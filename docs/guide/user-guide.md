# User Guide

TaskSmack is a cross-platform system monitor and task manager built with modern C++23, Dear ImGui, OpenGL, and SDL3. It delivers an immediate-mode UI with real-time process and system metrics, designed for developers and power users who want accurate, low-overhead monitoring on both Windows and Linux.

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
| `optimized` / `win-optimized` | x86-64-v3 (AVX2) | 2013+ (Haswell / Excavator) |

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

Column visibility is toggled via the column header context menu and persisted across sessions.

**Sorting** is available on any column with a single click. Click again to reverse order.

**Filtering** narrows the list to processes whose names or command lines match typed text.

**Tree view** shows the parent–child process hierarchy when enabled.

Process rows are colour-coded by state (running, sleeping, stopped, zombie).

### System Metrics

The System Metrics panel displays real-time and historical charts for:

- **CPU utilisation** — system-wide and per-core breakdowns
- **Memory** — used, cached, and available RAM displayed as percentages over time
- **Swap** — swap usage percentage history
- **Load average** (Linux only) — 1, 5, and 15-minute load averages
- **I/O wait** (Linux only) — percentage of CPU time spent waiting for I/O

All charts retain a scrolling history window (configurable length) implemented as decimating ring buffers, so memory usage stays bounded regardless of how long the app runs.

### Network Monitoring

The Network panel provides three levels of visibility:

| Level | What is shown |
|-------|---------------|
| System-wide | Total sent/received bytes per second across all interfaces |
| Per-interface | Individual interface throughput with status and link speed |
| Per-process | Bytes sent and received attributed to each process |

An interface selector lets you focus on a specific adapter. Per-process network rates are **lifetime averages** (total bytes since the process was first seen divided by elapsed time), not instantaneous deltas.

### Battery / Power Monitoring

On systems with a battery, TaskSmack shows:

- Charge percentage
- Current power consumption in watts
- Estimated time remaining (discharge) or time to full (charging)
- Battery health percentage

Power readings follow the convention: **positive watts = discharging/consuming**, negative watts = battery charging.

### GPU Monitoring

GPU metrics are loaded at runtime from vendor libraries — no recompilation needed:

| Vendor | Library | Metrics |
|--------|---------|---------|
| NVIDIA | NVML (`libnvidia-ml.so` / `nvml.dll`) | Utilisation %, memory used/total, temperature, power draw |
| AMD | ROCm SMI (`librocm_smi64.so`) | Utilisation %, memory, temperature |
| Intel | DRM/sysfs (Linux), DXGI (Windows) | Enumeration and basic info |

**Per-process GPU utilisation** sums utilisation across all GPUs, so a process working across two GPUs can legitimately show GPU% > 100 %.

If the vendor library is not found at runtime the GPU section is hidden automatically — no error is shown.

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

---

## Platform Differences

The following table summarises capabilities that differ between Windows and Linux. The UI hides unavailable items automatically; no configuration is needed.

| Feature | Linux | Windows |
|---------|-------|---------|
| CPU utilisation (total + per-core) | ✅ | ✅ |
| Memory metrics | ✅ | ✅ |
| System uptime | ✅ | ✅ |
| Process I/O counters | ✅ (requires root / `CAP_DAC_READ_SEARCH`) | ✅ (no elevated privileges needed) |
| Thread count per process | ✅ | ✅ |
| Process priority (nice) | ✅ | ✅ (mapped −20 … +19) |
| Process terminate / kill | ✅ | ✅ |
| Process stop / resume (SIGSTOP/SIGCONT) | ✅ | ❌ |
| I/O wait time (`iowait`) | ✅ | ❌ (Windows concept does not exist) |
| Steal time (`steal`) | ✅ | ❌ |
| Load average (1/5/15 min) | ✅ | ❌ |
| Shared memory per process | ✅ (`/proc/[pid]/statm`) | ❌ |
| NVIDIA GPU metrics | ✅ (NVML) | ✅ (NVML) |
| AMD GPU metrics | ✅ (ROCm SMI) | ⚠️ (partial) |
| Intel GPU enumeration | ✅ (DRM/sysfs) | ✅ (DXGI) |

---

## Configuration

TaskSmack persists settings in several places:

| Setting | Location |
|---------|----------|
| Window layout, panel positions, ImGui state | `imgui.ini` next to the executable (or in the user config directory) |
| Active theme | Saved in application settings (TOML) |
| Column visibility | Persisted in `imgui.ini` via ImGui's table storage |
| User themes | `%APPDATA%\TaskSmack\themes\` (Windows) or `~/.config/tasksmack/themes/` (Linux) |

To reset all layout settings, delete `imgui.ini`. To reset theme settings, remove the theme entry from the application settings file.
