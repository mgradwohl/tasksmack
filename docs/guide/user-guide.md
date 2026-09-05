# User Guide

TaskSmack is a cross-platform system monitor and task manager built with modern C++23, Dear ImGui, OpenGL, and SDL3. It delivers an immediate-mode UI with real-time process and system metrics, designed for developers and power users who want accurate, low-overhead monitoring on both Windows and Linux.

---

## Supported Platforms

- Linux
- Windows 10 or later

macOS and other operating systems are not currently supported.

---

## Download & Install

**[→ Latest Release](https://github.com/mgradwohl/tasksmack/releases/latest)**

| Platform | Package | Install |
|----------|---------|---------|
| Linux (Debian/Ubuntu) | `.deb` | `sudo dpkg -i tasksmack-*.deb` |
| Linux (other) | `.tar.gz` | Extract and run `bin/TaskSmack` |
| Windows | `.zip` | Extract and run `TaskSmack.exe` |

---

## System Requirements

### CPU Compatibility

TaskSmack ships several build flavours targeting different CPU generations. Pick the package that matches your hardware:

| Preset | Target microarchitecture | Minimum CPU |
|--------|--------------------------|-------------|
| `release` / `win-release` | Default compiler optimizations | Any x86-64 |
| `release-compatible` / `win-release-compatible` | x86-64-v2 | 2009+ (Core i3/i5/i7, Athlon II) |
| `optimized` / `win-optimized` | x86-64-v3 (AVX2) | Intel Haswell 2013+ / AMD Excavator 2015+ |

> **Tip:** If you see an "Illegal instruction" crash immediately on launch, your CPU does not support AVX2. Download the `release-compatible` build instead.

---

## Feature Overview

### Process Table

The process table is the primary view. It lists all running processes with htop-inspired columns:

- **CPU%** — percentage of total CPU time consumed since the last sample
- **MEM%** — percentage of physical RAM used
- **RES / VIRT / SHR** — resident, virtual, and shared memory sizes
- **TIME+** — cumulative CPU time
- **PPID** — parent process ID
- **NI** — nice/priority value
- **Threads** — thread count per process
- **Command** — full command line
- **I/O rates** — read and write bytes per second
- **Network rates** — sent and received bytes per second when attribution is available
- **GPU** — utilization and memory when the active backend supports per-process data
- **Affinity** — allowed CPU cores

Column visibility is toggled via the column header context menu and persisted across sessions.

**Sorting** is available on any column with a single click. Click again to reverse order.

**Filtering** narrows the list to processes whose names or command lines match typed text.

**Tree view** shows the parent–child process hierarchy when enabled.

Process rows are color-coded by state (running, sleeping, stopped, zombie).

### System Metrics

The System Metrics panel displays real-time and historical charts for:

- **CPU utilisation** — system-wide and per-core breakdowns
- **Memory** — used and cached RAM displayed as percentage history, with current availability derived from the latest system snapshot
- **Swap** — swap usage percentage history
- **Storage** — aggregate and per-device throughput
- **Network** — aggregate and per-interface throughput, totals, status, and link speed
- **GPU** — device utilization, memory, temperature, power, clocks, and engine data when available
- **Battery** — charge, power flow, remaining time, and health when present
- **Load average** (Linux only) — 1, 5, and 15-minute load averages
- **I/O wait** (Linux only) — percentage of CPU time spent waiting for I/O

All charts retain a bounded scrolling history window. Depending on the metric, TaskSmack uses fixed-capacity ring buffers or time-trimmed history containers so memory usage stays bounded regardless of how long the app runs.

### Network Monitoring

The System Overview and process views provide three levels of visibility:

| Level | What is shown |
|-------|---------------|
| System-wide | Total sent/received bytes per second across all interfaces |
| Per-interface | Individual interface throughput with status and link speed |
| Per-process | Bytes sent and received attributed to each process |

An interface selector lets you focus on a specific adapter. Per-process network rates are **lifetime averages** (total bytes since the process was first seen divided by elapsed time), not instantaneous deltas.

Linux per-process attribution uses Netlink and requires Linux 4.2 or later. Windows per-process attribution uses TCP EStats and requires administrator privileges to enable collection. System-wide and interface metrics remain available when process attribution is unavailable.

### Battery / Power Monitoring

On systems with a battery, TaskSmack shows:

- Charge percentage
- Current power consumption in watts
- Estimated time remaining (discharge) or time to full (charging)
- Battery health percentage

Power readings follow the convention: **positive watts = discharging/consuming**, negative watts = battery charging.

### GPU Monitoring

TaskSmack combines operating-system GPU APIs with optional vendor libraries:

| Vendor | Linux backend | Windows backend |
|--------|---------------|-----------------|
| NVIDIA | NVML (`libnvidia-ml.so`) | NVML (`nvml.dll`) with DXGI/PDH fallback data |
| AMD | ROCm SMI (`librocm_smi64.so`) | DXGI/PDH capability-dependent data |
| Intel/generic | DRM/sysfs | DXGI/PDH |

**Per-process GPU utilisation** sums utilisation across all GPUs, so a process working across two GPUs can legitimately show GPU% > 100 %.

The UI shows only the metrics exposed by the available backend. If no backend discovers a usable GPU, GPU sections are hidden.

### Process Actions

Right-click any process row to access actions:

| Action | Linux | Windows |
|--------|-------|---------|
| Terminate (SIGTERM / graceful) | ✅ | ✅ |
| Kill (SIGKILL / force) | ✅ | ✅ |
| Stop (SIGSTOP / suspend) | ✅ | ❌ |
| Resume (SIGCONT) | ✅ | ❌ |
| Change priority (nice) | ✅ | ✅ (mapped) |

Destructive actions require confirmation.

### Themes and Configuration

TaskSmack uses TOML-based theme files. Built-in themes are bundled with the application. To add your own:

1. Create a `.toml` file using the same key structure as a built-in theme.
2. Drop it in the **user themes folder** for your platform:

| Platform | Path |
|----------|------|
| Windows | `%APPDATA%\TaskSmack\themes\` |
| Linux | `~/.config/tasksmack/themes/` |

3. Restart the application — your theme will appear in the theme selector.

TaskSmack currently ships 20 themes. Theme schema details and the current inventory live in [`assets/themes/README.md`](https://github.com/mgradwohl/tasksmack/blob/main/assets/themes/README.md).

---

## Platform Differences

The following table summarises capabilities that differ between Windows and Linux. The UI hides unavailable items automatically; no configuration is needed.

| Feature | Linux | Windows |
|---------|-------|---------|
| CPU utilisation (total + per-core) | ✅ | ✅ |
| Memory metrics | ✅ | ✅ |
| System uptime | ✅ | ✅ |
| Process I/O counters | ✅ (requires root / `CAP_DAC_READ_SEARCH`) | ✅ (no elevated privileges needed) |
| Per-process network | ✅ (Linux 4.2+ Netlink) | ✅ (TCP EStats; administrator required) |
| Thread count per process | ✅ | ✅ |
| Process priority (nice) | ✅ | ✅ (mapped −20 … +19) |
| Process terminate / kill | ✅ | ✅ |
| Process stop / resume (SIGSTOP/SIGCONT) | ✅ | ❌ |
| I/O wait time (`iowait`) | ✅ | ❌ (Windows concept does not exist) |
| Steal time (`steal`) | ✅ | ❌ |
| Load average (1/5/15 min) | ✅ | ❌ |
| Shared memory per process | ✅ (`/proc/[pid]/statm`) | ❌ |
| NVIDIA GPU metrics | ✅ (NVML) | ✅ (NVML) |
| AMD GPU metrics | ✅ (ROCm SMI) | Capability-dependent via DXGI/PDH |
| Intel/generic GPU | ✅ (DRM/sysfs) | ✅ (DXGI/PDH) |
| Border resize cursors with custom title bar | ✅ (app-managed client-side cursors, same as Windows) | ✅ (app-managed client-side cursors) |
| Double-click title bar to maximize/restore | Works on X11/XWayland; not on native Wayland (use the maximize button there instead) | ✅ |
| Native OS window decorations instead of custom title bar | Opt-in, native Wayland only (Settings > Advanced) | Not available (custom title bar always used) |

---

## Configuration

TaskSmack persists settings in several places:

| Setting | Location |
|---------|----------|
| Window state, column visibility, theme, font, sampling, history, and advanced metric/UI settings | `config.toml` in the user config directory (`%APPDATA%\TaskSmack\` on Windows, `~/.config/tasksmack/` on Linux) |
| User themes | `%APPDATA%\TaskSmack\themes\` (Windows) or `~/.config/tasksmack/themes/` (Linux) |

To reset all layout and theme settings, delete the `config.toml` file in the user config directory. TaskSmack will recreate it with defaults on the next launch.
