# TaskSmack

[![Latest Release](https://img.shields.io/github/v/release/mgradwohl/tasksmack?style=flat-square)](https://github.com/mgradwohl/tasksmack/releases/latest)
[![CI (PR)](https://github.com/mgradwohl/tasksmack/actions/workflows/ci.yml/badge.svg?event=pull_request)](https://github.com/mgradwohl/tasksmack/actions/workflows/ci.yml)
[![Sanitizers](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/sanitizers.yml?branch=main&style=flat-square&label=Sanitizers)](https://github.com/mgradwohl/tasksmack/actions/workflows/sanitizers.yml)
[![Static Analysis](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/static-analysis.yml?branch=main&style=flat-square&label=Static%20Analysis)](https://github.com/mgradwohl/tasksmack/actions/workflows/static-analysis.yml)
[![CodeQL](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/codeql.yml?branch=main&style=flat-square&label=CodeQL)](https://github.com/mgradwohl/tasksmack/actions/workflows/codeql.yml)
[![OSV Scan](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/osv-scanner.yml?branch=main&style=flat-square&label=OSV%20Scan)](https://github.com/mgradwohl/tasksmack/actions/workflows/osv-scanner.yml)
[![Pre-commit](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/pre-commit.yml?branch=main&style=flat-square&label=Pre-commit)](https://github.com/mgradwohl/tasksmack/actions/workflows/pre-commit.yml)
[![Codecov](https://img.shields.io/codecov/c/github/mgradwohl/tasksmack?style=flat-square)](https://codecov.io/gh/mgradwohl/tasksmack)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/mgradwohl/tasksmack/badge)](https://securityscorecards.dev/viewer/?uri=github.com/mgradwohl/tasksmack)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)

TaskSmack is a cross-platform system monitor / task manager built with modern C++23, Dear ImGui, OpenGL, and SDL3.

## Build Matrix Status

Main-branch build badges are shown below; the CI badge above is filtered to pull request validation, even though the workflow also runs on `dev/**` pushes.

| Platform | Debug | Release |
|----------|-------|---------|
| Linux | [![Linux Debug](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-linux-debug.yml?branch=main&style=flat-square&label=Linux%20Debug)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-linux-debug.yml) | [![Linux Release](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-linux-release.yml?branch=main&style=flat-square&label=Linux%20Release)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-linux-release.yml) |
| Windows | [![Windows Debug](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-windows-debug.yml?branch=main&style=flat-square&label=Windows%20Debug)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-windows-debug.yml) | [![Windows Release](https://img.shields.io/github/actions/workflow/status/mgradwohl/tasksmack/build-windows-release.yml?branch=main&style=flat-square&label=Windows%20Release)](https://github.com/mgradwohl/tasksmack/actions/workflows/build-windows-release.yml) |

## Download

**[→ Latest Release](https://github.com/mgradwohl/tasksmack/releases/latest)**

| Platform | Package | Install |
|----------|---------|---------|
| Linux (Debian/Ubuntu) | `.deb` | `sudo dpkg -i tasksmack-*.deb` |
| Linux (other) | `.tar.gz` | Extract and run `bin/TaskSmack` |
| Windows | `.zip` | Extract and run `TaskSmack.exe` |

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
- System metrics with history graphs (CPU, memory, swap, per-core)
- Network monitoring with per-interface breakdown and dedicated Network Panel
  - System-wide and per-interface throughput graphs
  - Per-process network tracking (sent/received bytes per second)
  - Interface selector with status and link speed display
- Battery/power monitoring (charge %, power consumption, time remaining, health)
- Configurable themes (TOML-based) and user themes: drop .toml files in your user config themes folder (Windows: %APPDATA%/TaskSmack/themes, Linux: ~/.config/tasksmack/themes)
- Strict layered architecture (Platform → Domain → UI) for testable metrics math and clean OS boundaries

## For Developers

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.

- Use the **issue templates** for bug reports and feature requests
- PRs will be checked against the **PR template** checklist
- Security issues should be reported per [SECURITY.md](SECURITY.md)
- Architecture overview: [tasksmack.md](tasksmack.md)
- Security policy: [SECURITY.md](SECURITY.md)
- Changelog: [CHANGELOG.md](CHANGELOG.md)

## License

MIT License - See [LICENSE](LICENSE) file.
