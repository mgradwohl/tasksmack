# Implemented Features

This is the canonical inventory of user-visible and engineering features currently implemented in TaskSmack. Partial capabilities are called out explicitly; planned work belongs in [tasksmack.md](tasksmack.md).

## Process Monitoring and Control

| Feature | Current behavior |
|---------|------------------|
| Process table | Sortable, filterable list with PID, PPID, name, command, user, state/status, CPU, memory, CPU time, priority, thread/handle counts, affinity, page faults, I/O, network, and GPU columns where supported |
| Process tree | Collapsible parent/child hierarchy with cached rebuilds |
| Process details | Overview and capability-driven detail sections for CPU, memory, I/O, network, and GPU data |
| Column configuration | Header context menu controls visibility; settings persist in `config.toml` |
| Stable process identity | PID and process start time prevent counter reuse when an operating system recycles a PID |
| Process actions | Terminate, force terminate, and priority changes on Linux and Windows; stop/resume on Linux |
| Disk I/O rates | Per-process read/write rates; Linux access may require root or `CAP_DAC_READ_SEARCH` |
| Network rates | Lifetime-average per-process sent/received rates; Linux uses Netlink, Windows uses TCP EStats when running as administrator |
| GPU attribution | Per-process utilization, memory, device, and engine data when supported by the active GPU backend |

## System Monitoring

| Feature | Current behavior |
|---------|------------------|
| CPU | Total and per-core utilization, breakdowns, and bounded history |
| Memory and swap | Current usage, availability, cache data, swap, and bounded history |
| Storage | System and per-device throughput and history |
| Network | System-wide and per-interface throughput, totals, status, and link speed in the System Overview |
| GPU | Per-device utilization, memory, temperature, power, clocks, fan, encoder/decoder data when exposed by the backend |
| Battery and power | Charge, power flow, time remaining/time to full, and health when a battery is present |
| Linux-only metrics | Load averages, I/O wait, steal time, and shared process memory |

## User Experience and Configuration

| Feature | Current behavior |
|---------|------------------|
| Three-tab shell | System Overview, Processes, and Process Details |
| Docking and viewports | Dear ImGui docking with multi-viewport support |
| Themes | 20 bundled TOML themes plus user themes |
| Persistent settings | Theme, font size, process columns, sampling, history, window state, and advanced metric/UI settings |
| Sampling controls | Refresh interval from 100 ms to 5 seconds; default 1 second |
| History controls | Retention from 10 seconds to 30 minutes; default 5 minutes |
| Capability-aware UI | Unsupported metrics and actions are hidden; reduced privileges can trigger an elevation notice |
| Responsive sampling | Process enumeration uses `BackgroundSampler`; GPU refresh uses a dedicated worker; snapshot versions and background tree building avoid redundant process-list copies and UI stalls |
| Resize pipeline | Batched SDL3 event handling, event-driven viewport updates, idle throttling, and minimized throttling |

## Platform Backends

| Capability | Linux | Windows |
|------------|-------|---------|
| Process/system metrics | `/proc`, sysfs, Netlink, POSIX APIs | Native system and process APIs |
| Process I/O | `/proc/[pid]/io`, permission-dependent | `GetProcessIoCounters` |
| Per-process network | Netlink `INET_DIAG`, Linux 4.2+ | TCP EStats, administrator privileges required |
| NVIDIA GPU | NVML | NVML |
| AMD GPU | ROCm SMI | DXGI/PDH capability-dependent data |
| Intel/generic GPU | DRM/sysfs | DXGI/PDH |
| Stop/resume | `SIGSTOP` / `SIGCONT` | Not supported |
| Minimum supported OS | Supported Linux distributions with required runtime libraries | Windows 10 |

## Engineering Infrastructure

| Item | Current behavior |
|------|------------------|
| Build system | CMake 3.29+, Ninja, C++23, Clang/LLVM 22, Linux libc++, Windows MSVC STL |
| Workflow presets | One-command development, coverage, sanitizer, and benchmark workflows |
| Automated setup | `tools/setup-dev.sh` and `tools/setup-dev.ps1` |
| Cleanup | Cross-platform clean scripts with dry-run and extended cleanup modes |
| Tests | Google Test suites across Platform, Domain, Core, UI, App, and integration boundaries |
| Benchmarks | Google Benchmark coverage for models, history, formatting, numeric helpers, and Linux Netlink |
| Static quality | clang-format, clang-tidy, pre-commit, IWYU guidance, warnings-as-errors |
| Runtime analysis | ASan/UBSan, TSan, MSan preset, coverage, perf/heaptrack, ETW, resize tracing |
| Build analysis | Clang time-trace collection and unity-build presets |
| Optimization | PCH, compiler caching, IPO/LTO, CPU compatibility presets, PGO workflows |
| Packaging | Linux `.deb`/`.tar.gz`, Windows `.zip`, generated version resources, SPDX-JSON SBOM |
| CI and security | Linux/Windows builds, coverage thresholds, CodeQL, OSV, dependency review, Scorecard, sanitizer and static-analysis workflows |
| Releases | Strict SemVer tags drive packaging and a git-cliff changelog pull request |
