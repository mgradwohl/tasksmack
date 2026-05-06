# TaskSmack

[![CI](https://github.com/mgradwohl/tasksmack/actions/workflows/ci.yml/badge.svg)](https://github.com/mgradwohl/tasksmack/actions/workflows/ci.yml)
[![CodeQL](https://github.com/mgradwohl/tasksmack/actions/workflows/codeql.yml/badge.svg)](https://github.com/mgradwohl/tasksmack/actions/workflows/codeql.yml)
[![OSV Dependency Scan](https://github.com/mgradwohl/tasksmack/actions/workflows/osv-scanner.yml/badge.svg)](https://github.com/mgradwohl/tasksmack/actions/workflows/osv-scanner.yml)
[![Latest Release](https://img.shields.io/github/v/release/mgradwohl/tasksmack?sort=semver)](https://github.com/mgradwohl/tasksmack/releases/latest)
[![License](https://img.shields.io/github/license/mgradwohl/tasksmack)](LICENSE)
[![Pre-commit](https://github.com/mgradwohl/tasksmack/actions/workflows/pre-commit.yml/badge.svg)](https://github.com/mgradwohl/tasksmack/actions/workflows/pre-commit.yml)
[![Last Commit](https://img.shields.io/github/last-commit/mgradwohl/tasksmack)](https://github.com/mgradwohl/tasksmack/commits/main)
[![Downloads](https://img.shields.io/github/downloads/mgradwohl/tasksmack/total)](https://github.com/mgradwohl/tasksmack/releases)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)

TaskSmack is a cross-platform system monitor / task manager built with modern C++23, Dear ImGui, OpenGL, and SDL3.

## Why TaskSmack

- Cross-platform (Windows + Linux)
- ImGui-based UI with docking and multi-viewport support
- Process table with htop-inspired columns (CPU%, MEM%, RES, VIRT, SHR, TIME+, PPID, NI, threads, command, I/O rates)
- Per-process disk I/O rates (read/write bytes per second)
  - Windows: Available via `GetProcessIoCounters` (no special privileges required)
  - Linux: Requires root or `CAP_DAC_READ_SEARCH` capability to read `/proc/[pid]/io`
- Process state color-coding
- Process actions: terminate (SIGTERM), kill (SIGKILL), stop/resume (SIGSTOP/SIGCONT on Linux)
- Column visibility toggles (persisted)
- System metrics with history graphs (CPU, memory, swap, per-core)
- Network monitoring with per-interface breakdown and dedicated Network Panel
  - System-wide and per-interface throughput graphs
  - Per-process network tracking (sent/received bytes per second)
  - Interface selector with status and link speed display
- Battery/power monitoring (charge %, power consumption, time remaining, health)
- Configurable themes (TOML-based) and user themes: drop `.toml` files in your user config themes folder (Windows: `%APPDATA%/TaskSmack/themes`, Linux: `~/.config/tasksmack/themes`)
- Strict layered architecture (Platform → Domain → UI) for testable metrics math and clean OS boundaries

## System Requirements

### CPU Requirements

TaskSmack provides multiple build configurations optimized for different CPU generations:

- **Standard builds** (`release`, `win-release`): Use default compiler optimizations, compatible with most x86-64 CPUs
- **Compatible builds** (`release-compatible`, `win-release-compatible`): Target x86-64-v2 microarchitecture (2009+, Core i3/i5/i7, Athlon II)
- **Optimized builds** (`optimized`, `win-optimized`): Target x86-64-v3 microarchitecture (2013+, Haswell/Excavator), requires AVX2 support

**Note:** If you encounter "Illegal instruction" errors when running binaries built with the `optimized` preset, your CPU may not support AVX2. Use the `release-compatible` preset instead for broader compatibility.

## Documentation

README is the primary landing page today and is intentionally structured so it can be promoted to a GitHub Pages home page later without major rework.

- **Project overview and features:** [README.md](README.md)
- **Contributor workflow (canonical):** [CONTRIBUTING.md](CONTRIBUTING.md)
- **Architecture and engineering notes (canonical):** [tasksmack.md](tasksmack.md)
- **Completed features list (canonical):** [completed-features.md](completed-features.md)
- **Security policy:** [SECURITY.md](SECURITY.md)
- **Changelog:** [CHANGELOG.md](CHANGELOG.md)

## For Developers

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.

- Use the **issue templates** for bug reports and feature requests
- PRs are checked against the **PR template** checklist
- Security issues should be reported per [SECURITY.md](SECURITY.md)

## License

MIT License - See [LICENSE](LICENSE) file.
