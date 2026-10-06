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
| Linux (Debian/Ubuntu) | `.deb` | `sudo apt install ./TaskSmack-*.deb`, then run `TaskSmack` (installs to `/usr/bin`) |
| Linux (other) | `.tar.gz` | Extract and run `bin/TaskSmack` |
| Windows | `.zip` | Extract and run `TaskSmack.exe` |

### Verifying a Release

Every file on a release (the `.deb`, `.tar.gz`, `.zip`, and the SBOM) is signed with
[Sigstore](https://www.sigstore.dev/) via keyless OIDC signing in the `release` GitHub Actions
workflow. Each asset has a matching `<filename>.bundle` alongside it on the same release —
download both, then verify with [cosign](https://github.com/sigstore/cosign):

```bash
cosign verify-blob \
  --bundle <asset>.bundle \
  --certificate-identity-regexp '^https://github\.com/mgradwohl/tasksmack/\.github/workflows/release\.yml@refs/tags/.+$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  <asset>
```

Replace the filename with whichever asset you downloaded. The two `--certificate-*` flags pin
verification to TaskSmack's actual release workflow and to GitHub's OIDC issuer, so a signature
from any other source fails to verify. A successful check prints `Verified OK`.

If a release asset has **no matching `.bundle` file**, or `cosign verify-blob` **fails**, do not
trust that asset — treat it as potentially tampered with. Re-download from the
[Releases page](https://github.com/mgradwohl/tasksmack/releases/latest) rather than a mirror, and
if the problem persists, report it per [SECURITY.md](https://github.com/mgradwohl/tasksmack/blob/main/SECURITY.md).

---

## System Requirements

### CPU Compatibility

The packages on the [Releases page](https://github.com/mgradwohl/tasksmack/releases/latest) are built with the `release` / `win-release` preset, which uses default compiler optimizations and runs on any x86-64 CPU — no AVX2 or other extended instruction set is required.

Building from source lets you target other microarchitectures instead:

| Preset | Target microarchitecture | Minimum CPU |
|--------|--------------------------|-------------|
| `release` / `win-release` | Default compiler optimizations | Any x86-64 |
| `release-compatible` / `win-release-compatible` | x86-64-v2 | 2009+ (Core i3/i5/i7, Athlon II) |
| `optimized` / `win-optimized` | x86-64-v3 (AVX2) | Intel Haswell 2013+ / AMD Excavator 2015+ |

See [CONTRIBUTING.md](../../CONTRIBUTING.md#cpu-compatibility) for build instructions. If you build the `optimized` preset yourself and see an "Illegal instruction" crash immediately on launch, your CPU does not support AVX2 — use `release-compatible` instead.

---

## Feature Overview

### Process Table

The process table is the primary view. It lists all running processes with these columns:

- **State** — what the process is doing (Running, Sleeping, and so on). Windows has no process state of its own, so there it comes from the process's threads: Running if any thread is running or ready to run, Stopped if every thread is suspended (a suspended app), otherwise Sleeping. The System Idle Process is Idle, and a process with no threads to judge by (Secure System) is Unknown.
- **CPU %** — percentage of total CPU time consumed since the last sample
- **Mem %** — percentage of physical RAM used
- **Memory / Virtual / Shared / Peak Mem** — resident, virtual, shared, and peak resident memory sizes
- **CPU Time** — cumulative CPU time, as a duration ("45s", "2m 05s", "1h 02m")
- **PPID** — parent process ID
- **Priority** — scheduling priority (from the nice value)
- **Threads** — thread count per process
- **Page Faults** — cumulative page faults
- **Command** — full command line. On Windows, a process whose command line can't be read (System, Registry, isolated processes such as LsaIso.exe) shows its executable's path, or its name in brackets.
- **I/O rates** — read and write bytes per second
- **Network rates** — sent and received bytes per second when attribution is available
- **GPU %, GPU Mem, GPU Engine, GPU** — utilization, memory, engines, and which GPU, when the active backend supports per-process data
- **Affinity** — allowed CPU cores

Column visibility is toggled via the column header context menu and persisted across sessions.

A cell reading **-** is a value of 0 (or one that doesn't apply). A cell reading **N/A** is a value TaskSmack could not read for that process: on Linux, for other users' processes, the FD count without `CAP_DAC_READ_SEARCH`, and the I/O and network rates without both `CAP_DAC_READ_SEARCH` and `CAP_SYS_PTRACE` (root with its normal capabilities has both; see the FAQ); on Windows, without administrator rights, every process's network rates (handle counts and I/O rates are read for every process). Process Details shows the same values as N/A, with a gap in their charts, and the system totals leave them out. Sorting puts N/A below every reading.

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
- **GPU** — device utilization, memory, temperature, power, clocks, and engine data when available. Each GPU shows only the sensors it reports, so an integrated GPU beside a discrete one doesn't get the discrete GPU's power or fan charts. On Linux, a GPU that has gone to sleep to save power (common for the discrete GPU on hybrid laptops) is labelled **(Sleeping)** and is not sampled until it wakes, so TaskSmack's periodic updates don't keep it awake; its readings show N/A meanwhile. Detecting an NVIDIA GPU at startup doesn't wake it either: until it first wakes it is listed under the model name its driver reports (or "NVIDIA GPU"), and TaskSmack asks NVML about it only once it is awake. On WSL, in a container that hides the PCI devices, or with an NVIDIA driver too old to look a GPU up by PCI address, startup detection can still wake it once. A GPU that is asleep when TaskSmack starts shows the sensor charts its driver supports in general until it first wakes, then only its own. The Overview header's **VRAM** figure counts discrete GPUs only, since an integrated GPU's memory is system RAM
- **Battery** — charge, power flow, remaining time, and health when present
- **Load average** (Linux only) — 1, 5, and 15-minute load averages
- **I/O wait** (Linux only) — percentage of CPU time spent waiting for I/O

All charts retain a bounded scrolling history window. Depending on the metric, TaskSmack uses fixed-capacity ring buffers or time-trimmed history containers so memory usage stays bounded regardless of how long the app runs.

### Network Monitoring

The System Overview and process views provide three levels of visibility:

| Level | What is shown |
|-------|---------------|
| System-wide | Total sent/received bytes per second across the hardware interfaces |
| Per-interface | Individual interface throughput with status and link speed |
| Per-process | Bytes sent and received attributed to each process |

An interface selector lets you focus on a specific adapter. The Total leaves out virtual interfaces (on Linux, bridges such as `docker0`, `veth` pairs, VPN tunnels such as `wg0` or `tun0`, VLANs; on Windows, adapters Windows doesn't report as hardware, such as VPN adapters, WSL `vEthernet`, and WAN Miniports), because their traffic also crosses a hardware interface and counting both doubled it. They remain in the selector, marked "virtual, not in Total". If there is no hardware interface at all, as inside a container, every interface counts.

Per-process network rates are the bytes the process's TCP connections transferred between two readings, divided by the time between them. On Linux the readings are cached (`socket_stats_cache_ttl_ms`, 500 ms by default), and a refresh that reuses one shows the last rate (see the FAQ). UDP traffic, including QUIC/HTTP3, video calls, games, and DNS, is not attributed to processes on either platform. A browser streaming over HTTP/3 can show close to 0 B/s while the interface is busy.

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

**Intel GPUs on Linux** (i915 and xe drivers) report what the kernel exposes for each card:

- **Clock:** i915's `gt_cur_freq_mhz`, or xe's `tile0/gt0/freq0/cur_freq`.
- **Temperature and power:** from the card's hwmon, which only discrete cards (Arc) have. Temperature is the package sensor: the hwmon channel labelled `pkg` (xe: `temp2_input`), or else the lowest-numbered temperature input (i915: `temp1_input`). Power is worked out from hwmon's energy counter, so it appears from the second sample on.
- **VRAM:** comes from the DRM memory-region query on the card's render node (`/dev/dri/renderD*`), made only while the card is awake. The capacity is remembered after the first answer, and also tells a discrete card from an integrated one. Used VRAM appears only when the kernel reports it (i915 needs `CAP_PERFMON` for that); when it does, the query is repeated each sample to keep the figure current, otherwise it isn't made again.
- **Utilisation:** worked out from the engine busy time the kernel reports for each program that has the card open (`/proc/<pid>/fdinfo`, Linux 5.19+ for i915). The busiest engine class (render, copy, video, video enhance or compute) is shown, so it appears from the second sample on. TaskSmack looks for programs using the card every 10 seconds, so a program that has just started is counted within 10 seconds. Without root, TaskSmack can only see your own programs, so GPU work by other users' programs (or a display server running as root) isn't counted. On a kernel that doesn't report engine busy time, or where TaskSmack can't look into `/proc` at all (a sandbox that hides it, or denies every program's open files), utilisation shows N/A rather than 0%.
- **Sleeping cards:** a card in runtime suspend isn't queried, so watching it doesn't wake it.

**AMD GPUs on Linux** (ROCm SMI): an APU's integrated GPU is recognised from the graphics-core version amdgpu publishes in sysfs (`ip_discovery`), or from its PCI device ID on kernels without it, so it is labelled integrated and its shared memory isn't counted as VRAM. An APU generation newer than TaskSmack's list still shows as discrete.

**Per-process GPU figures** are counted the way the GPU tab counts each GPU. GPU% is the process's utilisation of the busiest GPU it uses (0–100 %). GPU memory counts dedicated memory on a discrete GPU and, on Windows, shared memory on an integrated one, added up across GPUs. Process Details also lists dedicated and shared memory separately when the process has shared memory, which only Windows reports.

The UI shows only the metrics exposed by the available backend. If no backend discovers a usable GPU, GPU sections are hidden.

On Linux, TaskSmack checks for GPU changes every 10 seconds without waking a sleeping GPU: a GPU that is hot-plugged (an eGPU) appears, and one that is removed, or lost after a driver reset or reload, is re-detected once it is back. A GPU that stays in the list keeps its chart history; one that is removed disappears from the GPU tab. On Windows the GPU list is still fixed at startup.

### Numbers and units

TaskSmack writes a quantity the same way wherever it appears: in a table cell, in the value strip beside a chart, in a chart tooltip and on a chart axis.

Digits are all the same width, so a value that changes every second does not shift the text around it, and in the process table's size, rate and power columns the decimal points line up whatever the unit ("512.0 B" above "3.2 MiB").

- **Sizes and rates** use binary units with their IEC names: B, KiB, MiB, GiB and TiB (1 KiB = 1,024 bytes), with one decimal, such as "512.0 MiB" or "1.5 GiB/s". A network interface's link speed is shown as a rate in the same units ("119.2 MiB/s" for a 1 Gbps link), with its rated speed ("1 Gbps") beside it or on hover.
- **Percentages** are whole numbers from 10% up and keep one decimal below it ("4.2%"). Per-process CPU and memory percentages always keep one decimal, as the process table shows them.
- **Power** has one decimal in W, mW or µW ("45.0 W"). **Temperature** is in whole degrees, rounded ("65°C").
- **Durations** (CPU Time, uptime) use the two largest units: "45s", "2m 05s", "1h 02m", "3d 04h". The charts' time axis counts back from **now** ("5m", "4m", … "now"), and a chart tooltip gives the hovered sample's age.

### Process Actions

Right-click any process row to access actions:

| Action | Linux | Windows |
|--------|-------|---------|
| Terminate (SIGTERM / graceful) | ✅ | ✅ |
| Kill (SIGKILL / force) | ✅ | ✅ |
| Stop (SIGSTOP / suspend) | ✅ | ❌ |
| Resume (SIGCONT) | ✅ | ❌ |
| Change priority (nice) | ✅ | ✅ (mapped) |

Destructive actions require confirmation. In Process Details, Terminate and Kill, which end the process, are drawn in red, apart from Suspend and Resume.

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
| Process I/O counters | ✅ (other users' processes: `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE`, which root has with its normal capabilities; root alone isn't enough where capabilities are dropped) | ✅ (no elevated privileges needed) |
| Per-process network (TCP only) | ✅ (Linux 4.2+ Netlink; other users' processes: `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE`, which root has with its normal capabilities; root alone isn't enough where capabilities are dropped) | ✅ (TCP EStats; administrator required) |
| Thread count per process | ✅ | ✅ |
| Process priority (nice) | ✅ | ✅ (mapped −20 … +19) |
| Process terminate / kill | ✅ | ✅ |
| Process stop / resume (SIGSTOP/SIGCONT) | ✅ | ❌ |
| I/O wait time (`iowait`) | ✅ (shown as its own band; counted as idle, not busy, so CPU % matches Windows) | ❌ (Windows concept does not exist) |
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

### Window size and position

TaskSmack reopens at the size and position it had when it was closed, and maximized if it was maximized. Closing it while maximized keeps the size and position it had before it was maximized -- whether it was maximized with the title-bar button or by the window manager or compositor (a keyboard shortcut, a window menu, snapping) -- so Restore returns there on the next launch. (Native Wayland does not let apps position their windows, so there only the size and maximized state are restored.) On X11 and XWayland, the title-bar Maximize button asks the window manager to maximize the window when it supports that, so the window fills the same area as the window manager's own maximize and stops at the taskbar or panel.

If the saved position is no longer on any connected display (a monitor was unplugged, say), TaskSmack opens centered on the primary display instead, and a saved size larger than the display is shrunk to fit it.

### Settings dialog

The Settings dialog (the gear icon) has three sections:

- **Appearance:** Theme and Font size (Small to Largest).
- **Performance:** Update interval (how often values are sampled) and History length (how much the charts keep).
- **Advanced:** buttons that open `config.toml` and the user themes folder, and **Show limited-data notice**, which turns the startup notice about missing administrator or root rights back on after its "Don't show again" was ticked.

**Save** writes your changes to `config.toml` and closes the dialog; **Cancel** (or Escape) closes it without changing anything. **Reset to defaults** sets every control in the dialog back to its default, and Save keeps them.

Dialogs (Settings, About and the limited-data notice) are kept inside the main window. When the font size or display scaling makes the Settings dialog taller than the window, its options scroll and the Cancel and Save buttons stay visible.

To reset all layout and theme settings, delete the `config.toml` file in the user config directory. TaskSmack will recreate it with defaults on the next launch.

### Advanced settings (config.toml only)

These settings aren't in the Settings dialog. Edit them in `config.toml` while TaskSmack is closed; they are read at startup. Out-of-range values are clamped to the range shown.

| Key | Default | Range | Effect |
|-----|---------|-------|--------|
| `[metrics] max_sane_rate_bps` | 12500000000 (100 Gbps) | 1e9–1e11 bytes/s | A network rate above this is taken for a bad reading (such as a counter reset). An interface's rate is shown as 0 and is a gap in the system network chart (and in the all-interfaces total). A process's rate is shown as 0 and recorded as 0 in its charts and in the all-processes network totals, not as a gap. Raise it for links faster than 100 Gbps. Disk rates have their own fixed ceiling of 1 TB/s. |
| `[ui] chart_smooth_factor` | 0.5 | 0.0–0.95 | How slowly live values and the bars beside the charts follow each new sample, as a fraction of the refresh interval. Lower follows changes faster; 0 barely eases. |
| `[ui] chart_tau_ms_min` | 20 | 5–100 ms | The shortest easing time, used at fast refresh intervals. |
| `[ui] chart_tau_ms_max` | 400 | 100–2000 ms | The longest easing time, used at slow refresh intervals. |
| `[ui] chart_anti_aliasing` | true | true/false | Smooth chart line edges. Turn it off to save CPU/GPU time on integrated graphics. |
| `[sampling] socket_stats_cache_ttl_ms` | 500 | 0–5000 ms | Linux only. How long per-process network readings are cached. |

Older versions also wrote `[metrics] min_time_for_rate_seconds`, `[metrics] integrated_gpu_vram_threshold_mb`, `[ui] progress_color_low_threshold` and `[ui] progress_color_high_threshold`. None of them ever had an effect, and TaskSmack now removes them from `config.toml` the next time it saves. Network rates are measured over each interval, so no start-up delay is needed. Integrated and discrete GPUs are told apart by vendor (Windows) or by PCI bus and, for AMD, graphics-core version (Linux), not by a VRAM threshold. TaskSmack has no threshold-coloured progress bars.

### Running TaskSmack twice

Only one TaskSmack runs at a time for each config directory. Starting a second one shows "TaskSmack is already running", and the second one exits. Otherwise two copies saving the same `config.toml` could overwrite each other's settings. The check uses a `tasksmack.lock` file next to `config.toml`. The operating system releases the lock however TaskSmack exits, so you never need to delete the file. A Windows build and a WSL build on the same machine use different config directories, so both can run at once.
