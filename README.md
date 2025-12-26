# TaskSmack

TaskSmack is a cross-platform system monitor / task manager built with modern C++23, Dear ImGui, OpenGL, and GLFW.

## System Requirements

### CPU Requirements

TaskSmack provides multiple build configurations optimized for different CPU generations:

- **Standard builds** (`release`, `win-release`): Use default compiler optimizations, compatible with most x86-64 CPUs
- **Compatible builds** (`release-compatible`, `win-release-compatible`): Target x86-64-v2 microarchitecture (2009+, Core i3/i5/i7, Athlon II)
- **Optimized builds** (`optimized`, `win-optimized`): Target x86-64-v3 microarchitecture (2013+, Haswell/Excavator), requires AVX2 support

**Note:** If you encounter "Illegal instruction" errors when running binaries built with the `optimized` preset, your CPU may not support AVX2. Use the `release-compatible` preset instead for broader compatibility.

## Features

- Cross-platform (Windows + Linux)
- ImGui-based UI with docking and multi-viewport support
- Process table with htop-inspired columns (CPU%, MEM%, RES, VIRT, SHR, TIME+, PPID, NI, threads, command, I/O rates)
- Per-process disk I/O rates (read/write bytes per second)
  - Windows: Available via `GetProcessIoCounters` (no special privileges required)
  - Linux: Requires root or `CAP_DAC_READ_SEARCH` capability to read `/proc/[pid]/io`
- Process state color-coding
- Process actions: terminate (SIGTERM), kill (SIGKILL), stop/resume (SIGSTOP/SIGCONT on Linux)
- Column visibility toggles (persisted)
- Configurable themes (TOML-based) and user themes: drop .toml files in your user config themes folder (Windows: %APPDATA%/TaskSmack/themes, Linux: ~/.config/tasksmack/themes)
- Strict layered architecture (Platform → Domain → UI) for testable metrics math and clean OS boundaries

## For Developers

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.

- Use the **issue templates** for bug reports and feature requests
- PRs will be checked against the **PR template** checklist
- Security issues should be reported per [SECURITY.md](SECURITY.md)
- Architecture overview: [tasksmack.md](tasksmack.md)
- Process/metrics implementation notes: [tasksmack.md](tasksmack.md)
- Security policy: [SECURITY.md](SECURITY.md)

## License

MIT License - See [LICENSE](LICENSE) file.
