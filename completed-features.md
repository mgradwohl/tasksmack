# Completed Features

This file lists features that are already implemented in TaskSmack.

## Application Features

| Feature | Source | Notes |
|---------|--------|-------|
| **MEM% Column** | htop | Memory as percentage of total system RAM |
| **TIME+ Column** | htop | CPU time formatted as H:MM:SS.cc |
| **Command Column** | htop | Full command line from `/proc/[pid]/cmdline` |
| **Task Summary** | htop | "N processes, M running" in panel header |
| **VIRT Column** | htop | Virtual memory size |
| **NI (Nice) Column** | htop | Process nice value (-20 to 19) |
| **Thread Count Column** | htop | Number of threads per process |
| **PPID Column** | htop | Parent process ID |
| **SHR Column** | htop | Shared memory size |
| **State Color Coding** | htop | Color-code process states based on theme |
| **Status Column** | Task Manager | Display process status (Suspended, Efficiency Mode) |
| **Column Visibility Toggles** | btop++ | Right-click table header to show/hide columns; persisted to config |
| **Process Tree View** | btop++, htop | Hierarchical view showing parent-child relationships with collapsible nodes |
| **Peak Working Set** | Task Manager | Horizontal reference line on memory graph showing historical peak RSS; OS-provided on Windows, tracked on Linux |
| **CPU Affinity Column** | Task Manager | Shows which CPU cores a process can run on (e.g., "0-3", "0,2,4") |
| **Power Usage Column** | Task Manager | Process power consumption (infrastructure ready, platform implementations pending) |
| **Signal Sending / Process Actions** | btop++ | Send signals to processes: SIGTERM (terminate), SIGKILL (kill), SIGSTOP (stop), SIGCONT (resume); Linux uses `kill()`, Windows uses `TerminateProcess` (stop/resume not supported on Windows) |
| **Priority Adjustment** | Task Manager, htop | Change process nice value (-20 to 19) via slider in Process Details; Linux uses `setpriority()`, Windows uses `SetPriorityClass()` |
| **Per-Process Network Usage** | Task Manager | Network rate display (sent/received) in Process Details Overview tab; Linux implementation uses Netlink INET_DIAG to query TCP/UDP socket byte counters (requires Linux 4.2+); Windows implementation pending |
| **Per-Process I/O Rates** | btop++, Task Manager | Disk read/write rates (bytes/sec) for each process; requires elevated privileges on Linux |
| **Power/Battery Stats** | btop++ | Battery charge, power consumption, time remaining, health%; shown in System Overview tab when battery detected |
| **GPU Monitoring (Phase 1-7)** | Task Manager, btop++ | GPU infrastructure and UI complete; Platform layer with NVIDIA (NVML), Intel (DRM), AMD (ROCm) probes on Linux; Domain GPUModel with history and per-process GPU fields in ProcessSnapshot; UI surfaces include SystemMetricsPanel GPU section, ProcessesPanel GPU columns, and ProcessDetailsPanel GPU tab |
| **Network Monitoring** | btop++, Task Manager | Complete network monitoring implementation with system-wide and per-process tracking |
| **System-Wide Network Stats** | btop++ | Total RX/TX bytes and rates in System Overview; Linux via `/proc/net/dev`, Windows via `GetIfTable`/`GetIfTable2` |
| **Per-Interface Network Breakdown** | btop++ | Interface selector dropdown showing individual interface throughput, status, and link speed |
| **Network Panel** | btop++ | Dedicated panel with per-interface throughput graphs, current rates with auto-scaling units (B/s to GB/s), cumulative transfer totals, interface status and link speed |
| **Per-Process Network Tracking** | Task Manager | Network sent/received rates per process; Linux via Netlink INET_DIAG socket stats, Windows via `GetPerTcpConnectionEStats` |
| **Linux Interface Caching** | — | Link speed cached with 60s TTL to reduce sysfs I/O; thread-safe implementation |

## Developer Tooling / Infrastructure

| Item | Notes |
|------|-------|
| **CMake presets** | Presets for Debug/Release and platform variants |
| **Legacy scripts deprecated** | Consolidated tooling scripts |
| **VS Code integration** | Tasks and launch configs |
| **Precompiled headers (PCH)** | Faster builds |
| **Compiler caching support** | ccache/sccache |
| **clang-tidy workflow** | Helper scripts and curated config |
| **CI on Linux + Windows** | Build, tests, format check, clang-tidy, coverage |
| **Coverage reporting** | llvm-cov HTML reports |
| **CPack packaging** | ZIP/installer generation via CPack |
| **Version header generation** | Auto-generate `version.h` during configure |
| **Sanitizer presets** | ASan+UBSan, TSan on Linux |
| **Compiler warning configuration** | Tuned warnings and warnings-as-errors |
| **`std::print` adoption** | Type-safe, format-string-based output |
| **FreeType font rendering** | Sharper text rendering at small sizes using FreeType with LightHinting |
