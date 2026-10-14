#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
exportarprogramaprogramador.py - Exportador de Arquitectura y Códigos de SOMA-LANG y Coder IDE
                                 Ejecútalo desde la subcarpeta tools/ o raíz.
"""

import os
import sys
from pathlib import Path
from datetime import datetime

def find_project_root():
    cur = Path.cwd()
    if (cur / "Makefile").exists() and (cur / "src").exists():
        return cur
    parent = cur.parent
    if (parent / "Makefile").exists() and (parent / "src").exists():
        return parent
    print("Error: No se encontro la raiz de SOMA.")
    sys.exit(1)

def print_tree(root):
    lines = ["ESTRUCTURA GENERAL DEL SUBSISTEMA DE PROGRAMACION Y CODER IDE:\n" + "-"*80, "soma/"]
    for dirpath, dirnames, filenames in os.walk(root):
        rel = Path(dirpath).relative_to(root)
        if any(p in str(rel) for p in [".git", "build"]):
            continue
        parts = rel.parts
        level = len(parts)
        indent = "  " * level
        for f in sorted(filenames):
            if f.endswith(('.c', '.h', '.soma', '.py', '.txt', '.ld', '.md')):
                lines.append(f"{indent}├── {f}")
    lines.append("-" * 80 + "\n")
    return "\n".join(lines)

def main():
    root = find_project_root()
    output_path = root / "contexto_programa_programador.txt"

    target_files = [
        "src/apps/coder_app.h",
        "src/apps/coder_app.c",
        "src/lang/lang.h",
        "src/lang/lang.c",
        "src/lang/vm.h",
        "src/lang/vm.c",
        "src/lang/lexer.h",
        "src/lang/lexer.c",
        "src/agent/agent.h",
        "src/agent/agent.c",
        "src/agent/shell.h",
        "src/agent/shell.c",
        "src/ui/gui.h",
        "src/ui/gui.c",
        "src/icons/icon_coder.h",
        "Makefile",
        "workspace/pong.soma",
        "workspace/demo_arrays.soma",
        "workspace/demo_graficos.soma"
    ]

    now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    header = [
        "=" * 80,
        "CONTEXTO DE ARQUITECTURA: SOMA STUDIO & SUBSISTEMA DE PROGRAMACION .SOMA",
        "Sistema Operativo: SOMA v0.2 (64-bit Bare-Metal)",
        f"Raiz del proyecto: {root}",
        f"Fecha de generacion: {now_str}",
        "=" * 80,
        ""
    ]

    body = [print_tree(root)]

    exported_count = 0
    for rel_path in target_files:
        p = root / rel_path
        if p.exists():
            content = p.read_text(encoding="utf-8", errors="replace")
            size = p.stat().st_size
            body.append("=" * 80)
            body.append(f"ARCHIVO: {rel_path}")
            body.append(f"TAMANO: {size} bytes")
            body.append("=" * 80)
            body.append(content)
            body.append("\n")
            exported_count += 1

    full_text = "\n".join(header + body)
    output_path.write_text(full_text, encoding="utf-8")

    print("\n" + "="*80)
    print(f" [EXPORTADOR CODER OK] Archivo generado exitosamente:")
    print(f" -> {output_path}")
    print(f" Archivos exportados: {exported_count}")
    print(f" Tamano total: {len(full_text)} caracteres")
    print("="*80 + "\n")

if __name__ == "__main__":
    main()