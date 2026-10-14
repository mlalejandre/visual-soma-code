# 🧠 SOMA OS v0.2 — AI-Native Operating System

**SOMA OS** is an experimental 64-bit operating system project exploring an environment where the kernel, graphical desktop, programming language, compiler, virtual machine and AI-assisted application workflow are developed as part of the same system.

The project is written primarily in freestanding C and assembly and is designed to boot without Linux or Windows acting as a host operating system. Its current focus is low-level systems engineering, a native graphical environment and **SOMA-LANG**, a language whose programs are compiled to bytecode and executed by the system's virtual machine.

> **Project status:** experimental and actively evolving. Capabilities and compatibility depend on the current source tree and configuration. SOMA OS is not intended for production use or for running untrusted code.

## ✨ Highlights

- **Bare-metal architecture:** kernel and hardware-facing components, with an execution workflow based on the project's runner and virtualized hardware where configured.
- **Her-Aura desktop:** a cosmic graphical environment with an orbital dock, procedural visual effects and floating application windows.
- **SOMA-LANG:** a programming language designed for applications running inside SOMA OS.
- **Compiler and VM:** source parsing, bytecode generation, diagnostics and virtual-machine execution.
- **AI-assisted Coder workflow:** generate application source from a natural-language request, validate it, inspect compiler diagnostics and attempt repairs.
- **`.soma` applications:** save generated programs and launch them again from the SOMA environment.
- **Native GUI APIs:** application-facing operations for windows, drawing and input, depending on the functionality available in the current build.
- **Low-level services:** virtual filesystems, drivers, networking and agent components represented in the project source.

## 🧩 SOMA-LANG and the Coder workflow

SOMA-LANG is designed around the services provided by SOMA OS. The compiler analyses source code and translates it into an intermediate instruction format (**bytecode**). That bytecode is executed by the virtual machine integrated into the system; it should not be confused with native x86-64 or ARM64 machine code.

The development workflow combines several stages:

1. **Plan:** interpret the requested application and outline its structure.
2. **Generate:** produce source code in SOMA-LANG.
3. **Validate:** run the linter and compiler checks.
4. **Diagnose and repair:** pass compiler feedback to the agent and try to correct detected errors, when supported by the current configuration.
5. **Run:** launch the application after it passes the available checks.

The repair loop is bounded in the current prototype. A successful compile does not by itself prove that a program is free of runtime defects or security issues.

Examples demonstrated by the project include generating an interactive calculator and launching a Pong game from a `.soma` file.

## 🌌 Graphical environment

The Her-Aura desktop is designed around a native windowed environment and procedural cosmic visuals. Components represented in the codebase include:

- Orbital application dock and animated central vortex.
- Floating windows with movement, resizing and Z-order handling.
- Text and icon rendering components.
- Terminal and system inspection tools.
- Graphical applications and document-related utilities, depending on the current build.

## 🖥️ Applications and tools

The project has included the following applications or subsystems; availability may vary as development progresses:

- **SOMA Browser:** graphical browser prototype with HTML/CSS rendering capabilities.
- **SOMA Paint:** bitmap drawing canvas with drawing tools and image saving.
- **SOMA File Manager:** file browsing and metadata display.
- **SOMA Notepad:** graphical text editor.
- **SOMA Memory View:** inspection of agent memory and shared state.
- **Document Layout:** preview of structured documents.
- **Graphical terminal:** command execution and shell interaction.
- **SOMA Studio / Coder IDE:** interface for AI-assisted application generation and validation.

## 🤖 Agents and orchestration

SOMA has been designed with a multi-agent workflow and shared context mechanisms. Components described in the project include:

- **soma:** high-level orchestrator for breaking down requests and coordinating work.
- **mayordomo:** operational agent for system and filesystem tasks.
- **buscador:** research and retrieval-oriented agent.
- **maquetador:** document formatting and layout-oriented agent.
- **Blackboard IPC:** shared state intended for inter-agent context exchange.

Agent availability and external service requirements depend on the configuration used to run the project.

## 🌐 Networking and web services

Networking components represented in the codebase include virtualized network-device support, Ethernet, ARP, IPv4, ICMP, UDP and TCP functionality, as well as HTTP client/server components. The exact protocols and services available in a given build should be verified against the source and runtime logs.

If the configured runner exposes the HTTP dashboard on the host, open:

```text
http://localhost:8090/
```

## 💾 Filesystems and persistence

The project includes filesystem and virtual-filesystem components. SOMAFS02 is the project's filesystem format, and additional mechanisms have been developed for source inspection and experimental patch workflows. Persistence behaviour depends on the disk image and runner configuration in use.

## 🚀 Getting started

### Requirements

Check the current `Dockerfile`, `Makefile` and `ejecutar.py` for exact dependencies. Depending on the selected target, the setup may require Python 3, Docker, QEMU and a cross-compilation toolchain.

### Start the default configuration

From the project root, run:

```bash
python3 ejecutar.py
```

To select the AArch64 target, if supported by the current version:

```bash
python3 ejecutar.py --arch aarch64
```

Watch the runner output for build errors, missing dependencies and the actual address or status of any services it starts.

### Launch a `.soma` application

Inside SOMA OS's own terminal, a program can be launched using the system's `app` command. For example, if the file exists at the specified guest path:

```text
soma> app /workspace/pong.soma
```

This is a command for the terminal running **inside SOMA OS**, not a command to run directly in the macOS shell.

## 📂 Repository layout

The layout below is indicative; the current repository tree is the authoritative reference. Some paths may differ between revisions.

```text
.
├── Dockerfile          # Build environment, if configured
├── Makefile            # Build rules
├── ejecutar.py         # Build/run entry point
├── grub/               # Bootloader configuration, if applicable
├── src/
│   ├── arch/           # Architecture-specific boot and memory support
│   ├── core/           # Kernel core and fundamental services
│   ├── drivers/        # Hardware and virtual-device drivers
│   ├── fs/             # Filesystems and VFS
│   ├── net/            # Networking components
│   ├── ui/             # Desktop and graphics
│   ├── apps/           # System applications
│   └── agent/          # Agent orchestration and AI integration
└── docs/               # Technical documentation, if present
```

## 🔐 Security and limitations

SOMA OS is a low-level experimental system. Restrictions on paths, instruction budgets, linter checks and recovery mechanisms can reduce certain failure modes, but they are not a formal security guarantee. Prefer testing in an emulator or an isolated development environment. Do not commit API keys, access tokens, `.env` files, private configuration or other secrets.

## 🤝 Contributing

Issues, reproducible bug reports and technical suggestions are welcome. When reporting a problem, include the target architecture, execution configuration, steps to reproduce and relevant logs. Before submitting changes, ensure that generated disk images, build artefacts and local credentials have not been added unintentionally.

## 📄 License

See [`LICENSE`](LICENSE) for the terms that apply to this repository.

---

**GitHub:** https://github.com/mlalejandre/visual-soma-code
