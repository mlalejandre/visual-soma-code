#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/exportarprogramacion.py - Exportador del Subsistema de Programación SOMA-LANG
Actualiza el archivo de pasos a seguir y emite contexto_programacion.txt con
todos los archivos vinculados directa e indirectamente al motor de lenguaje en Ring 0.
"""

import os
import sys
import datetime

# ==============================================================================
# 1. CONTENIDO ACTUALIZADO DE LA HOJA DE RUTA
# ==============================================================================
PASOS_A_SEGUIR_TXT = """================================================================================
HOJA DE RUTA Y FASES: DESARROLLO DEL LENGUAJE NATIVO SOMA (SOMA-LANG / .soma)
================================================================================

FILOSOFÍA DE DISEÑO:
- Diseñado para máxima ergonomía tanto para programadores humanos como para LLMs.
- Sintaxis familiar estilo C/TypeScript/Rust con tolerancia sintáctica a scripts LLM.
- Primitivas cognitivas de primer nivel: los agentes y la memoria son tipos nativos.
- 100% Freestanding: corre en Ring 0 sin dependencias externas, sobre x86_64 y AArch64.

--------------------------------------------------------------------------------
FASE 1: FUNDAMENTOS DEL MOTOR EN RING 0 [COMPLETADA]
--------------------------------------------------------------------------------
[x] Crear estructura del subsistema src/lang/
[x] Implementar Máquina Virtual de Bytecode basada en pila (vm.h / vm.c)
[x] Diseñar el conjunto de Opcodes (aritmética, variables, saltos, syscalls SOMA, OP_SWAP)
[x] Implementar Lexer / Tokenizador base sin libc (lexer.h / lexer.c)
[x] Integrar comandos 'vm test' y 'run' en la shell del sistema operativo
[x] Validar ejecución de bytecode aritmético y de control en memoria física
[x] Blindaje de memoria: instancias estáticas en .bss para evitar desbordamiento de pila (#PF)

--------------------------------------------------------------------------------
FASE 2: PARSER Y EMISOR DE BYTECODE [COMPLETADA]
--------------------------------------------------------------------------------
[x] Analizador sintáctico descendente recursivo (Recursive Descent Parser)
[x] Reconocimiento de expresiones matemáticas y precedencia de operadores (*, /, +, -, ==, !=, <, >)
[x] Concatenación automática de cadenas y enteros con '+' ("Resultado: " + (a + b))
[x] Declaración de variables locales y asignaciones con auto-declaración tolerante
[x] Estructuras de control de flujo (if / else / while / { bloques })
[x] Ejecución end-to-end de archivos fuente (.soma) desde VFS en Ring 0

--------------------------------------------------------------------------------
FASE 3: PRIMITIVAS DE HARDWARE Y SISTEMA OPERATIVO [COMPLETADA]
--------------------------------------------------------------------------------
[x] Enlace nativo con el sistema de archivos: vfs.read(), vfs.write(), vfs.exists()
[x] Enlace con memoria y telemetría: sys.ram(), sys.ram_free(), sys.uptime()
[x] Primitiva de consola y refresco visual: sys.clear()
[x] Generador de números pseudo-aleatorios en Ring 0: sys.rand() basado en TSC
[x] Búferes ampliados a 4096 bytes para soportar scripts de gran escala sin truncamiento

--------------------------------------------------------------------------------
FASE 4: SINTAXIS COGNITIVA Y SIMBIOSIS CON AGENTES IA [COMPLETADA]
--------------------------------------------------------------------------------
[x] Primitivas nativas de orquestación de agentes: ask "mision", agent.ask("agente", "mision")
[x] Búsqueda semántica BM25 en memoria de Ring 0: mem.query("término") conectada a vfs_search_bm25
[x] Lectura y escritura en la Pizarra Compartida (Blackboard IPC): mem.read(), mem.write()
[x] Base de Conocimiento Viva del Compilador sembrada en /mem/soma_lang_manual.txt

--------------------------------------------------------------------------------
FASE 5: ENTORNO, REPL Y AGENTE PROGRAMADOR AUTÓNOMO [COMPLETADA]
--------------------------------------------------------------------------------
[x] Primitiva de entrada interactiva: let n = input("prompt") con detección automática de enteros
[x] Semicolons opcionales en línea de comandos y final de línea (EOF)
[x] Evaluación directa en shell: run 40 + 2; o run sys.uptime()
[x] Modo interactivo REPL en Ring 0 con persistencia de variables: comando 'run' o 'run repl'
[x] Agente Programador Autónomo: comando 'coder <peticion>' y 'run coder <peticion>'
[x] Compilador Resiliente a LLM: comentarios '#', auto-declaración de variables, bucles 'loop/break',
    bloques envolventes 'proc main()', omisión transparente de 'import' y llamadas 'io.print/input'
[x] Aplicación nativa interactiva verificada en Ring 0: /apps/calc.soma (Calculadora)

--------------------------------------------------------------------------------
FASE 6: SIGUIENTE FRONTERA - EXPANSIONES GRÁFICAS Y MULTI-AGENTE [EN PLANIFICACIÓN]
--------------------------------------------------------------------------------
[ ] Enlace con Framebuffer 2D para GUIs: fb.clear(), fb.rect(), fb.text()
[ ] Ventanas interactivas de aplicaciones .soma en el escritorio Her-Aura
[ ] Subrutinas y funciones de usuario reutilizables (fn suma(a, b) { return a + b; })
[ ] Estructuras dinámicas de datos: Arrays indexados [x, y, z] y tablas asociativas
================================================================================
"""

# ==============================================================================
# 2. ARCHIVOS DIRECTA E INDIRECTAMENTE VINCULADOS A SOMA-LANG
# ==============================================================================
RELACIONADOS = [
    # 1. Núcleo del lenguaje SOMA-LANG
    "src/lang/lang.h",
    "src/lang/lang.c",
    "src/lang/vm.h",
    "src/lang/vm.c",
    "src/lang/lexer.h",
    "src/lang/lexer.c",
    "src/lang.h",
    "src/agent/lang.h",

    # 2. Shell, Terminal y Comandos del Sistema
    "src/agent/shell.h",
    "src/agent/shell.c",
    "src/ui/console.h",

    # 3. Agentes de IA y Conexión LLM
    "src/agent/agent.h",
    "src/agent/agent.c",
    "src/agent/llm.h",
    "src/agent/llm.c",

    # 4. Sistema de Archivos VFS & Memoria Semántica
    "src/fs/fs.h",
    "src/fs/disk_layout.h",

    # 5. Núcleo del Kernel y Hardware Ring 0
    "src/core/kernel.c",
    "src/core/arch.h",
    "src/core/mem.h",
    "src/core/mem.c",

    # 6. Build, Scripts y Configuración
    "Makefile",
    "linker_x86_64.ld",
    "test_demo.soma",
    "pasosaseguirlenguajeprogramacion.txt",
]

# Directorios/archivos a ignorar en el árbol
TREE_IGNORE = {
    ".git", "build", "__pycache__", ".DS_Store",
    "soma.iso", "soma.elf", "hdd.img", "boot.o"
}


def find_repo_root():
    """Detecta la raíz del repositorio soma/"""
    curr = os.path.abspath(os.getcwd())
    if os.path.exists(os.path.join(curr, "src", "lang")):
        return curr
    parent = os.path.abspath(os.path.join(curr, ".."))
    if os.path.exists(os.path.join(parent, "src", "lang")):
        return parent
    return curr


def generate_tree(dir_path, prefix=""):
    """Genera una representación en árbol del proyecto"""
    lines = []
    try:
        entries = sorted(os.listdir(dir_path))
    except Exception:
        return lines

    filtered = [e for e in entries if e not in TREE_IGNORE and not e.endswith(".o")]

    for i, entry in enumerate(filtered):
        is_last = (i == len(filtered) - 1)
        connector = "└── " if is_last else "├── "
        full_path = os.path.join(dir_path, entry)

        lines.append(f"{prefix}{connector}{entry}")
        if os.path.isdir(full_path):
            sub_prefix = prefix + ("    " if is_last else "│   ")
            lines.extend(generate_tree(full_path, sub_prefix))

    return lines


def update_roadmap(repo_root):
    """Actualiza los archivos de pasos a seguir con el progreso real"""
    targets = [
        os.path.join(repo_root, "pasosaseguirlenguajeprogramacion.txt"),
        os.path.join(repo_root, "pasos a seguir.txt"),
        os.path.join(repo_root, "pasosaseguir.txt")
    ]

    for t in targets:
        # Si existe el archivo o es el principal oficial, lo actualizamos
        if os.path.exists(t) or t.endswith("pasosaseguirlenguajeprogramacion.txt"):
            try:
                with open(t, "w", encoding="utf-8") as f:
                    f.write(PASOS_A_SEGUIR_TXT)
                print(f"[+] Hoja de ruta actualizada en: {os.path.relpath(t, repo_root)}")
            except Exception as e:
                print(f"[!] Error al actualizar {t}: {e}")


def main():
    root = find_repo_root()
    out_file = os.path.join(root, "contexto_programacion.txt")

    print(f"[*] Raíz detectada: {root}")

    # 1. Actualizar el archivo de hoja de ruta
    update_roadmap(root)

    # 2. Iniciar exportación de contexto
    print("[*] Recopilando estructura y código de SOMA-LANG...")
    buffer = []

    sep_equal = "=" * 80
    sep_dash = "-" * 80
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    header = (
        f"{sep_equal}\n"
        "CONTEXTO TÉCNICO DE PROGRAMACIÓN: SOMA-LANG & SUBSISTEMA DE COMPILACIÓN\n"
        f"Sistema Operativo: SOMA v0.2 (64-bit Bare-Metal C & ASM)\n"
        f"Raíz detectada: {root}\n"
        f"Fecha de exportación: {now_str}\n"
        f"{sep_equal}\n\n"
        "ESTRUCTURA GENERAL DEL PROYECTO:\n"
        f"{sep_dash}\n"
        "soma/\n"
    )
    buffer.append(header)

    tree_lines = generate_tree(root)
    buffer.append("\n".join(tree_lines))
    buffer.append(f"\n{sep_dash}\n\n")

    archivos_exportados = 0
    bytes_totales = 0

    for rel_path in RELACIONADOS:
        full_path = os.path.join(root, rel_path)
        if not os.path.exists(full_path):
            continue

        try:
            with open(full_path, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()

            size_bytes = len(content.encode("utf-8"))
            bytes_totales += size_bytes
            archivos_exportados += 1

            doc = (
                f"{sep_equal}\n"
                f"ARCHIVO: {rel_path}\n"
                f"TAMAÑO: {size_bytes} bytes\n"
                f"{sep_equal}\n"
                f"{content}\n\n\n"
            )
            buffer.append(doc)
            print(f"  -> Incluido: {rel_path} ({size_bytes} B)")
        except Exception as e:
            print(f"  [!] No se pudo leer {rel_path}: {e}")

    try:
        with open(out_file, "w", encoding="utf-8") as f:
            f.write("".join(buffer))
        print(f"\n[OK] Exportación exitosa a: {out_file}")
        print(f"     Total archivos: {archivos_exportados}")
        print(f"     Volumen de código: {bytes_totales / 1024:.2f} KiB")
    except Exception as e:
        print(f"[ERROR] No se pudo escribir {out_file}: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()