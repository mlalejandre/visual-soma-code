#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Parche SOMA: aumentar techo de salida y bloquear respuestas/codigo truncados.

Ejecutar desde la raiz de SOMA:
  python3 parche_llm_completo.py --dry-run
  python3 parche_llm_completo.py

Este parche no sustituye la auditoria del cliente HTTP src/agent/llm.c, cuyo
contenido no se incluye en el volcado aportado. Si el receptor lee solo parte
 del cuerpo HTTP, esa capa debe corregirse con su implementacion real.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
from pathlib import Path
import shutil
import sys
import tempfile

MARKER = "SOMA_LLM_COMPLETENESS_GUARD_V1"

HELPERS = r'''/* SOMA_LLM_COMPLETENESS_GUARD_V1
 * Detecta JSON incompleto y evita escribir programas .soma truncados.
 * Se ejecuta antes de dispatch_agent_command: el VFS nunca recibe ese cuerpo.
 */
static uint32_t llmfix_len(const char *s)
{
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int llmfix_count(const char *hay, const char *needle)
{
    int count = 0;
    uint32_t nlen = llmfix_len(needle);
    const char *p = hay;
    if (!hay || !needle || !nlen) return 0;
    while ((p = find_substr(p, needle)) != 0) { ++count; p += nlen; }
    return count;
}

/* Valida que exista un objeto JSON equilibrado, ignorando llaves dentro de strings.
 * Acepta texto explicativo antes/despues; el parser actual sigue extrayendo campos.
 */
static int llmfix_json_complete(const char *s)
{
    const char *p = s;
    char stack[64];
    uint32_t depth = 0;
    int in_string = 0, escaped = 0, started = 0, closed = 0;
    if (!s) return 0;
    while (*p) {
        char c = *p++;
        if (!started) {
            if (c != '{') continue;
            started = 1;
            stack[depth++] = '{';
            continue;
        }
        if (closed) continue;
        if (in_string) {
            if (escaped) { escaped = 0; continue; }
            if (c == '\\') { escaped = 1; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == '{' || c == '[') {
            if (depth >= sizeof(stack)) return 0;
            stack[depth++] = c;
        } else if (c == '}' || c == ']') {
            char want = (c == '}') ? '{' : '[';
            if (!depth || stack[depth - 1] != want) return 0;
            --depth;
            if (depth == 0) closed = 1;
        }
    }
    return started && closed && !in_string && !escaped && depth == 0;
}

static int llmfix_is_soma_write(const char *cmd)
{
    const char *p = cmd;
    const char *start;
    uint32_t n = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (!(p[0] == 'w' && p[1] == 'r' && p[2] == 'i' &&
          p[3] == 't' && p[4] == 'e' && (p[5] == ' ' || p[5] == '\t'))) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\'' || *p == '"') ++p;
    start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\'' && *p != '"') { ++p; ++n; }
    return n >= 6 && n >= 5 && start[n-5] == '.' &&
           (start[n-4] == 's' || start[n-4] == 'S') &&
           (start[n-3] == 'o' || start[n-3] == 'O') &&
           (start[n-2] == 'm' || start[n-2] == 'M') &&
           (start[n-1] == 'a' || start[n-1] == 'A');
}

/* Solo acepta write <ruta.soma> '<programa>' con delimitadores balanceados.
 * No intenta demostrar semantica; compile/lint siguen siendo obligatorios.
 */
static int llmfix_soma_write_complete(const char *cmd)
{
    const char *p = cmd, *q, *body, *end;
    char outer, quote = 0, stack[256];
    uint32_t depth = 0;
    int escaped = 0, in_line_comment = 0;
    if (!llmfix_is_soma_write(cmd)) return 1;
    while (*p == ' ' || *p == '\t') ++p;
    p += 5;
    while (*p == ' ' || *p == '\t') ++p;
    /* Saltar primero la ruta; la comilla del cuerpo aparece despues del espacio. */
    if (*p == '\'' || *p == '\"') {
        char path_quote = *p++;
        while (*p && *p != path_quote) { if (*p == '\\' && p[1]) ++p; ++p; }
        if (*p != path_quote) return 0;
        ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t') ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\'' && *p != '\"') return 0;
    outer = *p++;
    body = p;
    end = cmd + llmfix_len(cmd);
    while (end > body && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    if (end <= body || end[-1] != outer) return 0;
    --end; /* excluye comilla externa de cierre */
    if (end <= body) return 0;

    /* '...' es un marcador prohibido en el perfil SOMA Coder. */
    for (q = body; end - q >= 3; ++q)
        if (q[0] == '.' && q[1] == '.' && q[2] == '.') return 0;

    for (q = body; q < end; ++q) {
        char c = *q;
        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            continue;
        }
        if (quote) {
            if (escaped) { escaped = 0; continue; }
            if (c == '\\') { escaped = 1; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && q + 1 < end && q[1] == '/') { in_line_comment = 1; ++q; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{' || c == '(' || c == '[') {
            if (depth >= sizeof(stack)) return 0;
            stack[depth++] = c;
        } else if (c == '}' || c == ')' || c == ']') {
            char want = (c == '}') ? '{' : ((c == ')') ? '(' : '[');
            if (!depth || stack[depth - 1] != want) return 0;
            --depth;
        }
    }
    if (quote || escaped || depth) return 0;
    /* Rechaza finales típicos cortados aunque los delimitadores anteriores cierren. */
    while (end > body && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    if (end <= body) return 0;
    if (end[-1] == '=' || end[-1] == '+' || end[-1] == '-' || end[-1] == '*' ||
        end[-1] == '/' || end[-1] == ',' || end[-1] == '.') return 0;
    return 1;
}

static void llmfix_save_debug(const char *path, const char *text)
{
    uint32_t n = llmfix_len(text);
    if (path && text && n) (void)vfs_write(path, text, n);
}

'''

JSON_ANCHOR = r'''        if (!llm_chat_json(agent_prompt_buf, agent_reply_buf, 32768, 60000)) {
            kprint("Error en comunicacion estructurada con el agente.\n");
            break;
        }
'''
JSON_REPLACEMENT = JSON_ANCHOR + r'''
        /* No extraer ni ejecutar campos si el cuerpo JSON del LLM llego truncado. */
        if (!llmfix_json_complete(agent_reply_buf)) {
            const char *why = "LLM-TRUNCATED-JSON: respuesta incompleta o JSON desequilibrado; no se ejecuto ninguna accion. Reintenta con un JSON mas corto y una unica accion.";
            llmfix_save_debug("/tmp/llm_respuesta_incompleta.json", agent_reply_buf);
            history_add_note(history_buf, &hist_len, 8192, step, why);
            kprint("\n[LLM TRUNCADO]: JSON incompleto; respuesta guardada en /tmp/llm_respuesta_incompleta.json. Accion no ejecutada.\n");
            if (llmfix_count(history_buf, "LLM-TRUNCATED-") >= 3) {
                const char *fail = "[LLM RESPUESTA INCOMPLETA] Tres respuestas llegaron truncadas o como JSON invalido. No se ejecuto la accion parcial. Revisa /tmp/llm_respuesta_incompleta.json y la capa HTTP del cliente LLM.";
                uint32_t rp = 0, sl = 0;
                agent_last_response[0] = '\0';
                fb_puts(agent_last_response, sizeof(agent_last_response), &rp, fail);
                while (fail[sl]) ++sl;
                (void)vfs_write("/agent/blackboard.txt", fail, sl);
                kprint(fail); kprint("\n");
                finished = 1;
                break;
            }
            continue;
        }
'''

CMD_ANCHOR = r'''        llm_json_get(agent_reply_buf, "verdict", verdict, 32768);
'''
CMD_REPLACEMENT = CMD_ANCHOR + r'''

        /* Una respuesta JSON puede estar completa y contener un programa cortado.
         * En ese caso se rechaza ANTES de dispatch_agent_command/write al RamFS. */
        if (str_eq_ci(agent->name, "coder") && llmfix_is_soma_write(cmd_f) &&
            !llmfix_soma_write_complete(cmd_f)) {
            const char *why = "LLM-TRUNCATED-CODE: write .soma incompleto (comillas o delimitadores sin cerrar, o placeholder). No se escribio. Reenvia el programa completo en una sola accion write; reduce thought y no incluyas explicaciones.";
            llmfix_save_debug("/tmp/llm_comando_write_rechazado.txt", cmd_f);
            history_add_note(history_buf, &hist_len, 8192, step, why);
            kprint("\n[LLM TRUNCADO]: bloqueada escritura .soma incompleta; diagnostico en /tmp/llm_comando_write_rechazado.txt.\n");
            if (llmfix_count(history_buf, "LLM-TRUNCATED-") >= 3) {
                const char *fail = "[CODER NO VERIFICADO] Tres respuestas generaron JSON/codigo incompleto. Ningun write parcial adicional se ejecutara. Revisa /tmp/llm_comando_write_rechazado.txt y el cliente HTTP LLM.";
                uint32_t rp = 0, sl = 0;
                agent_last_response[0] = '\0';
                fb_puts(agent_last_response, sizeof(agent_last_response), &rp, fail);
                while (fail[sl]) ++sl;
                (void)vfs_write("/agent/blackboard.txt", fail, sl);
                kprint(fail); kprint("\n");
                finished = 1;
                break;
            }
            continue;
        }
'''

SERVER_CONTEXT_ANCHOR = 'CTX = int(os.getenv("REMOTE_CTX", "32768"))\nTHREADS = int(os.getenv("REMOTE_THREADS", "8"))\n'
SERVER_CONTEXT_REPLACEMENT = SERVER_CONTEXT_ANCHOR + 'PREDICT = int(os.getenv("REMOTE_PREDICT", "16384"))\n'
SERVER_ARGS_OLD = '        "-n", "8192",\n'
SERVER_ARGS_NEW = '        "-n", str(PREDICT),\n'


def atomic_write(path: Path, data: bytes) -> None:
    mode = path.stat().st_mode
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    except Exception:
        try:
            os.unlink(name)
        except OSError:
            pass
        raise


def prepare_agent(path: Path) -> tuple[str, bool]:
    original = path.read_text(encoding="utf-8")
    if MARKER in original:
        return original, False
    # Source versions differ after previous patches; these two stable anchors must be unique.
    if original.count(JSON_ANCHOR) != 1:
        raise RuntimeError("src/agent/agent.c: no encuentro exactamente una llamada llm_chat_json esperada")
    if original.count(CMD_ANCHOR) != 1:
        raise RuntimeError("src/agent/agent.c: no encuentro exactamente una extraccion del campo verdict")
    if original.count("void agent_run(const char *agent_name, const char *mission)") != 1:
        raise RuntimeError("src/agent/agent.c: no encuentro el punto de insercion de helpers")
    patched = original.replace("void agent_run(const char *agent_name, const char *mission)", HELPERS + "void agent_run(const char *agent_name, const char *mission)", 1)
    patched = patched.replace(JSON_ANCHOR, JSON_REPLACEMENT, 1)
    patched = patched.replace(CMD_ANCHOR, CMD_REPLACEMENT, 1)
    if patched.count(MARKER) != 1:
        raise RuntimeError("src/agent/agent.c: comprobacion interna del parche fallida")
    return patched, True


def prepare_server(path: Path) -> tuple[str, bool]:
    original = path.read_text(encoding="utf-8")
    marker = "# SOMA_PREDICT_CONFIG_V1"
    if marker in original:
        return original, False
    if original.count(SERVER_CONTEXT_ANCHOR) != 1:
        raise RuntimeError(f"{path}: no encuentro exactamente una configuracion CTX/THREADS esperada")
    if original.count(SERVER_ARGS_OLD) != 1:
        raise RuntimeError(f"{path}: no encuentro exactamente el limite de salida '-n 8192'")
    new = original.replace(SERVER_CONTEXT_ANCHOR, SERVER_CONTEXT_REPLACEMENT + marker + "\n", 1)
    new = new.replace(SERVER_ARGS_OLD, SERVER_ARGS_NEW, 1)
    return new, True


def main() -> int:
    ap = argparse.ArgumentParser(description="Refuerza integridad del JSON/codigo LLM y configura salida llama-server")
    ap.add_argument("--root", type=Path, default=Path.cwd())
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    root = args.root.expanduser().resolve()
    agent_path = root / "src/agent/agent.c"
    if not agent_path.is_file():
        print(f"ERROR: no existe {agent_path}; ejecuta desde la raiz del repositorio.", file=sys.stderr)
        return 2

    try:
        agent_new, agent_changed = prepare_agent(agent_path)
        server_candidates = [root / "tools/llm_server.py", root / "tools/llmserver.py"]
        server_paths = [p for p in server_candidates if p.is_file()]
        server_updates: list[tuple[Path, str, bool]] = []
        for sp in server_paths:
            sn, sc = prepare_server(sp)
            server_updates.append((sp, sn, sc))
    except (OSError, UnicodeError, RuntimeError) as exc:
        print(f"ERROR en preflight: {exc}\nNo se modifico ningun archivo.", file=sys.stderr)
        return 3

    if not server_paths:
        print("AVISO: no encontre tools/llm_server.py ni tools/llmserver.py; solo se podria proteger el lado del agente.", file=sys.stderr)
    print(f"Raiz: {root}")
    print(f"{'[CAMBIO]' if agent_changed else '[OK]'} src/agent/agent.c")
    print("  - Valida integridad JSON antes de extraer action/cmd.")
    print("  - Bloquea write .soma con strings o delimitadores incompletos.")
    print("  - Guarda muestras rechazadas bajo /tmp para depurar la respuesta LLM.")
    for sp, _, changed in server_updates:
        print(f"{'[CAMBIO]' if changed else '[OK]'} {sp.relative_to(root)}")
    if server_updates and any(changed for _, _, changed in server_updates):
        print("  - REMOTE_PREDICT configurable; predeterminado: 16384 tokens (antes 8192).")
    if args.dry_run:
        print("Simulacion terminada; no se escribio ningun archivo.")
        print("NOTA: no sustituye la correccion del receptor HTTP src/agent/llm.c si este trunca bytes.")
        return 0

    changed_files = []
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_dir = root / "backups" / f"parche_llm_completo_{stamp}"
    try:
        if agent_changed or any(changed for _, _, changed in server_updates):
            # Backup de todos los ficheros que se van a cambiar antes de escribir cualquiera.
            for path, _, changed in [(agent_path, agent_new, agent_changed)] + [(p, s, c) for p, s, c in server_updates]:
                if changed:
                    rel = path.relative_to(root)
                    dest = backup_dir / rel
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, dest)
            if agent_changed:
                atomic_write(agent_path, agent_new.encode("utf-8"))
                changed_files.append(agent_path)
            for sp, sn, sc in server_updates:
                if sc:
                    atomic_write(sp, sn.encode("utf-8"))
                    changed_files.append(sp)
    except Exception as exc:
        print(f"ERROR aplicando parche: {exc}", file=sys.stderr)
        print(f"Copias previas (si se crearon): {backup_dir}", file=sys.stderr)
        return 4

    if changed_files:
        print(f"Parche aplicado. Copias previas: {backup_dir}")
        for p in changed_files:
            print(f"  actualizado: {p.relative_to(root)}")
    else:
        print("No habia cambios pendientes; parche ya aplicado.")
    print("Siguiente paso: recompila SOMA y prueba un programa largo que antes se cortaba.")
    print("Si vuelve a salir JSON incompleto, revisa /tmp/llm_respuesta_incompleta.json; si se bloquea el source, revisa /tmp/llm_comando_write_rechazado.txt.")
    print("IMPORTANTE: para corregir un posible recv/Content-Length truncado hace falta revisar src/agent/llm.c y el lector HTTP real.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
