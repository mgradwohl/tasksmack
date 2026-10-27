# Implemented Features

This is the canonical inventory of user-visible and engineering features currently implemented in TaskSmack. Partial capabilities are called out explicitly; planned work belongs in [tasksmack.md](https://github.com/mgradwohl/tasksmack/blob/main/tasksmack.md).

## Process Monitoring and Control

| Feature | Current behavior |
|---------|------------------|
| Process table | Sortable, filterable list with PID, PPID, name and icon, command, user, publisher and type (Windows), container (Linux), services, state/status, CPU, memory (resident, virtual, shared, peak, and on Linux private and swap), CPU time, start time, priority, thread/handle/GDI counts, affinity, page faults, I/O, network, power (Linux with RAPL), GPU and NPU columns where supported; optional inline bars or sparklines behind the numeric columns; rows of processes that started in the last five seconds tinted, fading out (#3060) |
| Process tree | Collapsible parent/child hierarchy with cached rebuilds |
| Process details | Overview with Identity, Runtime and Actions cards, recent crashes, and on-demand sections for services, environment variables, connections, modules, security context and open files; capability-driven chart tabs for CPU, memory, I/O, network, GPU and NPU data |
| Column configuration | The header context menu and the toolbar's **Columns** menu (with **Reset columns**) control visibility; settings persist in `config.toml` |
| Stable process identity | PID and process start time prevent counter reuse when an operating system recycles a PID |
| Process actions | Terminate, force terminate, suspend/resume, priority and CPU affinity changes on Linux and Windows; I/O priority (ionice class and level) on Linux |
| Process dump files | "Create dump file..." in the Processes row menu and Process Details' Actions card (#807). On Linux, a core file written by gdb's `gcore` (offered only when `gcore` is installed), with the same confirm, folder choice (SDL's folder dialog), free-space warning, progress and Cancel. On Windows, a full-memory minidump (`MiniDumpWriteDump`, dbghelp.dll loaded from System32 on first use) on a worker thread, with progress, Cancel, Open folder, and clear messages for access denied, protected processes, 32-on-64 dumps and a full disk. Since #2001 it is written from a `PssCaptureSnapshot` clone, so the process pauses only for the snapshot (live, saying why, when the snapshot fails), to `%TEMP%` or a folder chosen with the confirm's "Save dump to..." (`IFileOpenDialog`, remembered as `[dumps] folder`), and the confirm warns early when the folder's drive may be too full |
| Privileged helper (Linux) | With the `.deb` (or an `.rpm` built from source), an action or read another user's process refuses (end, suspend, resume, priority, I/O priority, affinity; Process Details' environment, modules, open files, connections and PSS/USS) and System Information's root-only facts (firmware serials, UUID and SMBIOS version, memory modules, every user's crashes) offer **Authenticate and retry** / **Authenticate to read**: polkit asks for an administrator's password and `tasksmack-helper` does that request as root, from `/usr/libexec/tasksmack/` |
| Privileged helper (Windows) | When TaskSmack isn't running as administrator, an action Windows refuses for want of rights (Terminate, Kill, Suspend, Resume, priority, affinity) offers **Retry as administrator**, and Process Details' Modules, Open files and Connections offer **Read as administrator**: `TaskSmackHelper.exe` is started elevated through UAC and does that request. Offered only in Authenticode-signed releases (#2302) installed where only an administrator can change them, such as `C:\Program Files\TaskSmack`; see the FAQ's [Acting as an administrator on Windows](../guide/faq.md#acting-as-an-administrator-on-windows) |
| System call tracing | Linux only: the Actions block on Process Details' Overview opens a terminal running `strace -f -tt -p <PID>` (htop-style), started detached with no shell; disabled with the reason when strace or a terminal is missing, and refused with an explanation when ptrace (another user's process, Yama `ptrace_scope`) would not allow the attach |
| Multi-select and batch actions | Ctrl/Shift+click and Ctrl+A select several processes by identity; Suspend, Resume, Terminate and Kill act on all of them behind one confirmation, run on a worker with a progress count, and end in one summary result; one priority (nice value, or class on Windows) can be set on all of them the same way; **End process tree** ends a process and all its descendants, children first, the same way (#3059) |
| Disk I/O rates | Per-process read/write rates; Linux access to other users' processes needs `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE` in the effective set (root with its normal capabilities has them; root alone isn't enough where capabilities are dropped) |
| Network rates | Per-process sent/received rates over each sampling interval, TCP only by default; Linux uses Netlink, Windows uses TCP EStats when running as administrator, plus opt-in UDP from an ETW kernel network session |
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
| NPU | Utilization and memory of NPUs and other compute accelerators (Windows DXCore; Linux accel subsystem), kept apart from the GPU figures |
| Linux-only metrics | Load averages, pressure stall information (CPU, memory and I/O, #3062), I/O wait and steal time |

## User Experience and Configuration

| Feature | Current behavior |
|---------|------------------|
| Tabbed shell | Six tabs: the machine's metrics, Processes, Process Details, Services, Startup, and System (System Information) |
| Services | Windows Service Control Manager and Linux systemd services: list, start, stop, restart and set the startup type, with confirmations for critical services |
| Startup apps | Windows only: Run/RunOnce keys, Startup folders and packaged apps' startup tasks, enabled and disabled as Task Manager does |
| System Information | Operating system, firmware and board, memory modules, commit and paging, storage, security, sensors, graphics and displays, network adapters, boot performance, devices, drivers or kernel modules, and recent crashes; identifiers hidden until asked for; Copy and Save report (text or JSON) |
| Themes | 20 bundled TOML themes plus user themes |
| Persistent settings | Theme, font size, process columns, sampling, history, window state, and advanced metric/UI settings |
| Sampling controls | Refresh interval from 100 ms to 5 seconds; default 1 second |
| History controls | Retention from 10 seconds to 30 minutes; default 5 minutes |
| Keyboard shortcuts | htop-style F1 Help (shortcuts, column reference, tabs), F2 Settings, F5 tree view, F9 Kill (confirmed; every selected process with several selected), F10 Quit; Processes table navigation with arrows, j/k, Page Up/Down, Home/End, g/G, Left/Right to collapse/expand in tree view, Ctrl+A to select every row, and hold Ctrl to freeze the table |
| Capability-aware UI | Unsupported metrics and actions are hidden; reduced privileges can trigger an elevation notice |
| Responsive sampling | Separate process and system `BackgroundSampler` workers isolate workloads; immutable versioned System/Storage/GPU publications, process snapshot versions, and background tree building avoid render-frame locks, redundant copies, stale history entries, and UI stalls |
| Resize pipeline | Batched SDL3 event handling, event-driven viewport updates, idle throttling, and minimized throttling |

## Platform Backends

| Capability | Linux | Windows |
|------------|-------|---------|
| Process/system metrics | `/proc`, sysfs, Netlink, POSIX APIs | Native system and process APIs |
| Process I/O | `/proc/[pid]/io`, permission-dependent | `SystemProcessInformation` snapshot |
| Per-process network | Netlink `INET_DIAG`, Linux 4.2+ | TCP EStats, administrator privileges required |
| NVIDIA GPU | NVML | NVML |
| AMD GPU | ROCm SMI | DXGI/PDH capability-dependent data |
| Intel/generic GPU | DRM/sysfs | DXGI/PDH |
| Stop/resume | `SIGSTOP` / `SIGCONT` | `NtSuspendProcess` / `NtResumeProcess` on a start-time-checked handle; critical and protected processes, PID 4 and TaskSmack itself are never suspended (#2582) |
| Process environment variables | `/proc/[pid]/environ`, on demand for the selected process; secret-looking values masked until revealed | The environment block out of the process's PEB (`NtQueryInformationProcess(ProcessBasicInformation)`, `ReadProcessMemory`) on a start-time-checked handle, on demand for the selected process, capped at 1 MiB and 4096 variables; the same masking; 32-bit (WOW64) processes not yet (#2569) |
| Process security context | `/proc/[pid]/status` IDs, capabilities, `no_new_privs`, seccomp, LSM label and cgroup | The process token: user, integrity level, elevation, privileges and groups |
| Process dump files | gdb's `gcore` | `MiniDumpWriteDump` from a `PssCaptureSnapshot` clone |
| Per-process connections (netstat-style) | `/proc/[pid]/fd` socket inodes matched against `INET_DIAG` (fallback `/proc/[pid]/net/{tcp,tcp6,udp,udp6}`), on demand for the selected process | `GetExtendedTcpTable` / `GetExtendedUdpTable` owner-PID rows (IPv4 and IPv6) after a start-time check, on demand for the selected process (#1489) |
| Minimum supported OS | Supported Linux distributions with required runtime libraries | Windows 10 version 1903 |

## Engineering Infrastructure

| Item | Current behavior |
|------|------------------|
| Build system | CMake 3.29+, Ninja, C++23, Clang/LLVM 22, Linux libc++, Windows MSVC STL |
| Workflow presets | One-command development, coverage, sanitizer, and benchmark workflows |
| Automated setup | `tools/setup-dev.sh` and `tools/setup-dev.ps1` |
| Background polling | Centralized `BackgroundSampler` utilizing `ISamplable` for asynchronous model updates to avoid blocking the render thread |
| Cleanup | Cross-platform clean scripts with dry-run and extended cleanup modes |
| Tests | Google Test suites across Platform, Domain, Core, UI, App, and integration boundaries |
| Benchmarks | Google Benchmark coverage for models, history, formatting, numeric helpers, and Linux Netlink |
| Static quality | clang-format, clang-tidy, pre-commit, IWYU guidance, warnings-as-errors |
| Runtime analysis | ASan/UBSan, TSan, MSan preset, coverage, perf/heaptrack, ETW, resize tracing |
| Chart rendering | Shared `HistoryChart` RAII helper drives every ImPlot history chart; Render Metrics overlay (Ctrl+Shift+M) reports per-chart vertices and CPU time with CSV export, plus frame-level vertex/index/draw-call totals |
| Build analysis | Clang time-trace collection and unity-build presets |
| Optimization | PCH, compiler caching, IPO/LTO, CPU compatibility presets, PGO workflows |
| Packaging | Linux `.deb`/`.tar.gz` (`.rpm` when built locally with `rpmbuild`), Windows `.zip` and NSIS installer (signed, with a signed uninstaller), each for x64 and ARM64; generated version resources, SPDX-JSON SBOM |
| CI and security | Linux/Windows builds, coverage thresholds, CodeQL, OSV, dependency review, Scorecard, sanitizer and static-analysis workflows |
| Releases | Strict SemVer tags drive packaging and a git-cliff changelog pull request |
