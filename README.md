# TaskSmack

TaskSmack is a cross-platform system monitor / task manager built with modern C++23, Dear ImGui, OpenGL, and GLFW.

## Features

- Cross-platform (Windows + Linux)
- ImGui-based UI with docking and multi-viewport support
- Process table with htop-inspired columns (CPU%, MEM%, RES, VIRT, SHR, TIME+, PPID, NI, threads, command)
- Process state color-coding
- Column visibility toggles (persisted)
- Configurable themes (TOML-based)
- Strict layered architecture (Platform → Domain → UI) for testable metrics math and clean OS boundaries

## For Developers

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.

- Use the **issue templates** for bug reports and feature requests
- PRs will be checked against the **PR template** checklist
- Security issues should be reported per [SECURITY.md](SECURITY.md)
- Architecture overview: [tasksmack.md](tasksmack.md)
- Process/metrics implementation notes: [process.md](process.md)
- Security policy: [SECURITY.md](SECURITY.md)

## License

MIT License - See [LICENSE](LICENSE) file.
