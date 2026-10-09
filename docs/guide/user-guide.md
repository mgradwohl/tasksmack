# User Guide

TaskSmack is a system monitor and task manager for Linux and Windows, built with modern C++23, Dear ImGui, OpenGL, and SDL3. It delivers an immediate-mode UI with real-time process and system metrics, designed for developers and power users who want accurate, low-overhead monitoring.

---

## Supported Platforms

- Linux
- Windows 10 or later

TaskSmack runs on Linux and Windows only; macOS and other operating systems are not supported.

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

- **State** — what the process is doing (Running, Sleeping, and so on). Windows has no process state of its own, so there it comes from the process's threads: Running if any thread is running or ready to run, Stopped if every thread is suspended (a suspended app), otherwise Sleeping. The System Idle Process is Idle, and a process with no threads to judge by (Secure System) is Unknown. The column shows the state's one-letter code, as `ps` and `top` do: **R** Running, **S** Sleeping, **D** Disk Sleep (waiting on I/O), **Z** Zombie, **T** Stopped, **t** Tracing, **X** Dead, **I** Idle, **?** Unknown. Process Details spells the state out, in the same colour.
- **CPU %** — percentage of total CPU time consumed since the last sample
- **Mem %** — percentage of physical RAM used
- **Memory / Virtual / Shared / Peak Mem** — resident, virtual, shared, and peak resident memory sizes. On Windows, Memory is the private working set and Virtual the commit size, the figures Task Manager shows as Memory and Commit size; on Linux they are the resident set size (the resident field of `/proc/[pid]/statm`, shared pages included) and the whole address space (`vsize` from `/proc/[pid]/stat`). Peak Mem is the larger of the OS's high-water mark (Linux: `VmHWM`; Windows: peak working set) and the highest peak TaskSmack has seen for the process, so it can reach back before TaskSmack started. On Linux `VmHWM` resets when a process runs a new program (`exec`); TaskSmack keeps the higher peak it saw before the reset, but a peak from before an `exec` that happened before monitoring began is lost.
- **CPU Time** — cumulative CPU time, as a duration ("45s", "2m 05s", "1h 02m")
- **PPID** — parent process ID
- **Priority** — scheduling priority (from the nice value)
- **Threads** — thread count per process
- **Page Faults** — cumulative page faults. Windows keeps this count in 32 bits, so on a long-lived process it can wrap back to a small number; the page-fault rate counts through the wrap.
- **Command** — full command line. On Windows, a process whose command line can't be read (System, Registry, isolated processes such as LsaIso.exe) shows its executable's path, or its name in brackets.
- **I/O rates** — read and write bytes per second
- **Network rates** — sent and received bytes per second when attribution is available
- **GPU %, GPU Mem, GPU Engine, GPU** — utilization, memory, engines, and which GPU, when the active backend supports per-process data
- **Affinity** — the logical processors the process may run on, numbered from 0 as the operating system numbers them: ranges of three or more as `4-7`, others by number, e.g. `0-3,70`. On Linux these are the online processors the process may run on, 64 and above included (an offline processor is left out even when the process is allowed it; if the list of online processors can't be read, or a processor goes offline mid-sample so none of the allowed ones is online, the process's full allowed list is shown instead); on Windows it shows the process's primary processor group only.

Column visibility is toggled via the column header context menu and persisted across sessions. `config.toml` records only the columns you showed or hid; every other column follows the defaults, so a column added in a later version, or one this system can't fill (hidden by default), takes its default without you resetting anything. A config written by an older version listed every column: on the first launch of this version, a column saved at its default is taken as one you never changed and follows the defaults from then on.

**Inline meters:** the toolbar's **Columns › Show meter** submenu draws a bar behind the value in CPU %, Mem %, Memory, I/O Read, I/O Write, Net Sent, Net Received, GPU % and GPU Memory, as Task Manager shades its cells. The bar's length is the value's share and its colour deepens from the theme's `[process_meter] low` to `high` as it grows. Percentages are drawn against 0–100%; sizes and rates against the largest value in the list at the latest refresh. Only CPU % has a meter by default; your choices are saved as `[process_meters]` in `config.toml`, and **Reset columns** leaves them alone.

A cell reading **-** is a value of 0 (or one that doesn't apply). A cell reading **N/A** is a value TaskSmack could not read for that process: on Linux, for other users' processes, the FD count without `CAP_DAC_READ_SEARCH`, and the I/O and network rates without both `CAP_DAC_READ_SEARCH` and `CAP_SYS_PTRACE` (root with its normal capabilities has both; see the FAQ); on Windows, without administrator rights, every process's network rates (handle counts and I/O rates are read for every process). Process Details shows the same values as N/A, with a gap in their charts, and the system totals leave them out. Sorting puts N/A below every reading.

**Sorting** is available on any column with a single click. Click again to reverse order.

**Filtering** narrows the list to processes whose names or command lines match typed text.

**Tree view** shows the parent–child process hierarchy when enabled.

**Selecting several processes.** A click selects one process, and Process Details shows it. **Ctrl+click** adds a process to the selection, or removes it; **Shift+click** selects every row from the last one clicked to this one, in the order the rows are shown (sorted, filtered, and in tree view with collapsed branches skipped), and **Ctrl+Shift+click** adds that range to the selection. **Ctrl+A**, with the pointer over the table (or after clicking in it), selects every row shown. Every selected row is highlighted. Process Details keeps showing the process you clicked last. A process is selected as itself, not by its PID: when a selected process exits it drops out of the selection, and a new process that is later given the same PID is not selected. A keyboard move (arrows, Page Up / Down, Home / End) or a plain click selects just that one row again.

**Hold Ctrl to freeze** the table, as in Windows Task Manager. With the pointer over the process table (or after clicking in it), hold **Ctrl** and the rows stop updating and stop re-sorting, so the process you are aiming at stays under the pointer while you click it. A **Paused (Ctrl)** label appears beside the process count (just a pause icon when the window is too narrow for the words; hover it for the explanation), and releasing Ctrl resumes live updates at once. The freeze applies only to what the table shows: sampling carries on underneath, so charts and Process Details have no gaps, and the values shown while frozen are the last ones adopted, not live readings. Selecting a process, opening Process Details and process actions all work while frozen; an action on a process that has exited in the meantime fails just as it would without the freeze. Ctrl does not freeze while you are typing in the filter box, while TaskSmack's window is not focused, or when it is part of a shortcut such as Ctrl+= or Ctrl+Shift+M.

Process rows are color-coded by state (running, sleeping, stopped, zombie).

### Keyboard shortcuts

| Shortcut | Action |
|----------|--------|
| **F1** | Help: every keyboard shortcut, what each Processes column means, the tabs, and links to this guide and the issue tracker (see [Help and About](#help-and-about)) |
| **F2** | Settings |
| **F5** | Processes tab: switch between list and tree view |
| **F9** | Kill the selected process: opens the usual Kill confirmation, never kills without it (Processes and Process Details tabs). With several processes selected in the Processes table, asks to kill all of them |
| **F10** | Quit, the same as the window's Close button (settings are saved). F10 used to open the title bar's window menu; **Alt+Space** or **Ctrl+Space** still does |
| **Up** / **Down**, **k** / **j** | Processes table: select the previous / next row |
| **Page Up** / **Page Down** | Processes table: move one page up / down |
| **Home** / **End**, **g** / **G** | Processes table: select the first / last row |
| **Left** / **Right** (tree view) | Collapse the selected process, or if it is already collapsed (or has no children) go to its parent / expand it, or if it is already expanded go to its first child |
| **Ctrl+click** | Processes table: add a row to the selection, or remove it |
| **Shift+click** / **Ctrl+Shift+click** | Processes table: select the rows from the last one clicked to this one / add them to the selection |
| **Ctrl+A** (over the process table) | Select every row shown |
| Hold **Ctrl** (over the process table) | Freeze the process table while held |
| **Ctrl+=** / **Ctrl+-** (or **Ctrl+keypad +** / **Ctrl+keypad -**) | Increase / decrease the font size |
| **Ctrl+Shift+M** | Toggle the Render Metrics overlay and the status bar's FPS readout |

The table's keys work while the pointer is over the process table or after clicking in it. They move the selection through the rows in the order they are shown -- sorted, filtered, and in tree view with collapsed branches skipped -- and scroll the selected row into view; with no row selected (or the selected one filtered out or collapsed away), the first key selects the first row (**End** / **G**: the last). While the table has them, the arrow keys no longer move ImGui's own keyboard focus around it; **Tab** still does. The function keys (F1, F2, F5, F9, F10) and the process table's navigation keys do nothing while you are typing in the filter box or while a dialog or menu is open, and the function keys also do nothing with Ctrl, Shift, Alt or Super held. The older chords -- Ctrl+= / Ctrl+-, Ctrl+Shift+M and the window menu's Alt+Space / Ctrl+Space -- are not held back this way. **F9** acts only on a process you can see selected, and only on platforms that can kill; the confirmation names the process it was pressed for. With several processes selected, F9 asks to kill all of them, in one confirmation that lists them. Ctrl+A, like the navigation keys, does nothing while you are typing in the filter box (where it selects the typed text) or while a dialog or menu is open.

### Help and About

**F1**, or the **?** button in the title bar or the status bar, opens the **Help** window. It is not a dialog: you can keep it open beside the rest of TaskSmack, move it and resize it, and it keeps its place and size until you quit. It lists every keyboard shortcut grouped by where it works (Global, Processes, Process Details), with a box to find one by typing; describes each Processes column, with its units and platform notes, from the same text as the column headers' tooltips; gives one line on each tab; and links to this guide and the issue tracker. Escape or its close button closes it.

**About TaskSmack** (the version, build, commit, licences and credits) opens from the link at the foot of the Help window, or from **About TaskSmack** in Settings' Advanced section.

### System Metrics

The System Metrics panel displays real-time and historical charts for:

- **CPU Details** — the Overview opens with a block of CPU facts laid out like Task Manager's Performance › CPU page: utilization, kernel time, current speed, processes, threads, handles (file descriptors on Linux) and up time, then base speed, sockets, physical cores (split into performance and efficiency cores on a Windows hybrid CPU), logical processors, L1/L2/L3 cache sizes and installed memory. On Windows it also shows whether virtualization is enabled, whether a hypervisor is running, and whether virtualization-based security and memory integrity are on. A fact the system does not report shows as a muted dash with the reason on hover. The block uses as many label/value columns as the window is wide enough for and spans its full width, so a wide window spends less height on it. Click its heading to collapse it to one line with a summary (cores, base and current clock, utilization); TaskSmack remembers the choice (`[ui] cpu_details_expanded` in `config.toml`).
- **CPU utilisation** — system-wide and per-core breakdowns. The CPU Cores tab has a chart for each CPU reported since TaskSmack started, so a CPU that never comes online (reserved hot-add capacity, a CPU offline since boot) gets none, and one that goes offline keeps its chart, with a gap while it's offline. On a hybrid CPU (Intel Alder Lake and later, Arm big.LITTLE), each core's name has a marker for its kind: a bolt for a performance core, a leaf for an efficiency core, and a dimmer leaf with "LP" for a low-power efficiency core. Hover the marker or the name to see the kind in words. A CPU with one kind of core shows no marker.
- **Memory** — used and cached RAM displayed as percentage history, with current availability derived from the latest system snapshot
- **Swap** — swap usage percentage history
- **Storage** — aggregate and per-device throughput
- **Network** — aggregate and per-interface throughput, totals, status, and link speed
- **GPU** — device utilization, memory, temperature, power, clocks, and engine data when available. Each GPU shows only the sensors it reports, so an integrated GPU beside a discrete one doesn't get the discrete GPU's power or fan charts. A GPU that has gone to sleep to save power (common for the discrete GPU on hybrid laptops) is labelled **(Sleeping)** and its sensors are not read until it wakes, so TaskSmack's periodic updates don't keep it awake; its readings show N/A meanwhile. (On Windows its utilization and memory in use still show, since Windows reports them without waking the GPU.) On Windows an NVIDIA GPU that is awake but had no activity in the last update is not asked for its sensors either, so they don't stop it going to sleep: its temperature, power, clock and fan readings hold their last values, refreshed at least once a minute, until it is busy again. On Linux, detecting an NVIDIA GPU at startup doesn't wake it either: until it first wakes it is listed under the model name its driver reports (or "NVIDIA GPU"), and TaskSmack asks NVML about it only once it is awake. On Windows, on WSL, in a container that hides the PCI devices, or with an NVIDIA driver too old to look a GPU up by PCI address, startup detection can still wake it once. A GPU that is asleep when TaskSmack starts shows the sensor charts its driver supports in general until it first wakes, then only its own. The Overview's **GPU memory** figure (in CPU Details) counts discrete GPUs only, since an integrated GPU's memory is system RAM
- **Battery** — charge, power flow, remaining time, and health when present
- **Load average** (Linux only) — 1, 5, and 15-minute load averages
- **I/O wait** (Linux only) — percentage of CPU time spent waiting for I/O

All charts retain a bounded scrolling history window. Depending on the metric, TaskSmack uses fixed-capacity ring buffers or time-trimmed history containers so memory usage stays bounded regardless of how long the app runs.

A chart with several series has a value strip on its heading line as its key: a swatch and the current value of each series. (Grid cells such as the CPU core charts show their one value in the cell instead.) A series drawn against the chart's right-hand axis has **→** after its value ("Page Faults: 3.2K/s →"), in the colour of that axis's labels; its chart tooltip rows read the same way.

### Network Monitoring

The System Overview and process views provide three levels of visibility:

| Level | What is shown |
|-------|---------------|
| System-wide | Total sent/received bytes per second across the hardware interfaces |
| Per-interface | Individual interface throughput with status and link speed |
| Per-process | Bytes sent and received attributed to each process |

An interface selector lets you focus on a specific adapter. The Total leaves out virtual interfaces (on Linux, bridges such as `docker0`, `veth` pairs, VPN tunnels such as `wg0` or `tun0`, VLANs; on Windows, adapters Windows doesn't report as hardware, such as VPN adapters, WSL `vEthernet`, and WAN Miniports), because their traffic also crosses a hardware interface and counting both doubled it. They remain in the selector, marked "virtual, not in Total". If there is no hardware interface at all, as inside a container, every interface counts.

The Interface Status table's Sent and Received columns show the rate over the last refresh, not the smoothed rate beside the chart. A measured zero reads "0.0 B/s", muted. A muted "—" means there is no reading for that refresh, and hovering over it says why: the interface hasn't been sampled twice yet (just after starting, or a newly added interface), its byte counter went backwards (a driver reset), or it jumped by more than `max_sane_rate_bps`.

Per-process network rates are the bytes the process's TCP connections transferred between two readings, divided by the time between them. On Linux the readings are cached (`socket_stats_cache_ttl_ms`, 500 ms by default), and a refresh that reuses one shows the last rate (see the FAQ). UDP traffic, including QUIC/HTTP3, video calls, games, and DNS, is not attributed to processes on either platform. A browser streaming over HTTP/3 can show close to 0 B/s while the interface is busy.

Linux per-process attribution uses Netlink and requires Linux 4.2 or later. Windows per-process attribution uses TCP EStats and requires administrator privileges to enable collection. If Windows denies TCP EStats even to TaskSmack running as administrator (a policy or a driver can), the per-process network columns are hidden and a network icon in the status bar says so; running elevated can't bring them back on that system. System-wide and interface metrics remain available when process attribution is unavailable.

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
- **Utilisation:** worked out from the engine busy time the kernel reports for each program that has the card open (`/proc/<pid>/fdinfo`, Linux 5.19+ for i915). The busiest engine class (render, copy, video, video enhance or compute) is shown, so it appears from the second sample on. TaskSmack looks for programs using the card on the first refresh after every 10 seconds, and a newly found program counts from the refresh after that (its busy time needs two readings), so a program that has just started can take up to 10 seconds plus two refreshes to be counted. Without root, TaskSmack can only see your own programs, so GPU work by other users' programs (or a display server running as root) isn't counted. On a kernel that doesn't report engine busy time (i915 before Linux 5.19) while any program you can see has the card open, or where TaskSmack can't look into `/proc` at all (a sandbox that hides it, or denies every program's open files), utilisation shows N/A rather than 0%.
- **Sleeping cards:** a card in runtime suspend isn't queried, so watching it doesn't wake it.

**AMD GPUs on Linux** (ROCm SMI): an APU's integrated GPU is recognised from the graphics-core version amdgpu publishes in sysfs (`ip_discovery`), or from its PCI device ID on kernels without it, so it is labelled integrated and its shared memory isn't counted as VRAM. An APU generation newer than TaskSmack's list still shows as discrete.

**Per-process GPU figures** are counted the way the GPU tab counts each GPU. GPU% is the process's utilisation of the busiest GPU it uses (0–100 %). GPU memory counts dedicated memory on a discrete GPU and, on Windows, shared memory on an integrated one, added up across GPUs. Process Details also lists dedicated and shared memory separately when the process has shared memory, which only Windows reports.

The UI shows only the metrics exposed by the available backend. If no backend discovers a usable GPU, GPU sections are hidden.

TaskSmack checks for GPU changes every 10 seconds without waking a sleeping GPU: a GPU that is hot-plugged (an eGPU) appears, and one that is removed, or lost after a driver reset, reload or update, is re-detected once it is back. A GPU that stays in the list keeps its chart history, whatever is added or removed around it, and a newly added GPU doesn't take over a GPU that is still present; one that is removed disappears from the GPU tab. On Windows a GPU is known by where it sits on the PCI bus and by its model rather than by its place in the list, so the same card coming back after a driver reset is still the same GPU. That also means an identical card swapped into the same slot continues the old card's history, as if it had reconnected; a different model, or a card in a different slot, starts its own. On Windows, a change to the NVIDIA GPUs restarts NVIDIA's monitoring library (NVML), which can wake a sleeping NVIDIA GPU once, as starting TaskSmack can.

### Numbers and units

TaskSmack writes a quantity the same way wherever it appears: in a table cell, in the value strip beside a chart, in a chart tooltip and on a chart axis.

Digits are all the same width, so a value that changes every second does not shift the text around it, and in the process table's size, rate and power columns the decimal points line up whatever the unit ("512.0 B" above "3.2 MiB").

- **Sizes and rates** use binary units with their IEC names: B, KiB, MiB, GiB and TiB (1 KiB = 1,024 bytes), with one decimal, such as "512.0 MiB" or "1.5 GiB/s".
- **Link speed** is the exception: a network interface's link speed is in bits with decimal prefixes, as network adapters, switches and the OS describe it: "100 Mbit/s", "1 Gbit/s", "2.5 Gbit/s", "10 Gbit/s". Hovering over it shows the most it can carry in the units of the Sent and Received rates ("Up to 1.2 GiB/s" for a 10 Gbit/s link). An unknown link speed shows "-" in the Interface Status table and "Link: Unknown" beside the interface selector.
- **Percentages** are whole numbers from 10% up and keep one decimal below it ("4.2%"). Per-process CPU and memory percentages always keep one decimal, as the process table shows them.
- **Power** has one decimal in W, mW or µW ("45.0 W"). **Temperature** is in whole degrees, rounded ("65°C").
- **Durations** (CPU Time, uptime) use the two largest units: "45s", "2m 05s", "1h 02m", "3d 04h". The charts' time axis counts back from **now** ("5m", "4m", … "now"), and a chart tooltip gives the hovered sample's age.

### Environment variables

On Linux, the Process Details **Overview** tab has a collapsible **Environment** section, closed by default, under the Identity and Runtime blocks. Open it to list the selected process's environment variables in a NAME / VALUE table sorted by name; with more than 20 variables a filter box appears. A value too long for its column shows in a tooltip, up to its first 4 KiB (a longer value is cut there, at a character boundary); the cell itself draws at most the first 512 bytes. TaskSmack reads `/proc/[pid]/environ` only for the selected process and only while the section is open: once when you open it or select another process, then every 3 seconds. The list is the environment the process started with; a later `setenv()` inside the process does not show.

- **Secret-looking values are masked.** A variable whose name looks like it holds a secret shows `••••••••` instead of its value, with an eye button on its row that reveals that one value; it stays revealed until you select a different process (or press the button again). Masked values are never searched by the filter. The rule matches the name case-insensitively and errs toward masking:
  - masked if the name contains `TOKEN`, `SECRET`, `PASSWORD`, `PASSWD`, `PASSPHRASE`, `CREDENTIAL`, `COOKIE`, `PRIVATE`, `APIKEY`, `ACCESSKEY`, `SIGNINGKEY`, `CONNECTIONSTRING`, `CONNSTR`, `DATABASEURL` or `BEARER` anywhere (ignoring `_`, `-` and `.`);
  - masked if a word of the name (split at `_`, `-`, `.` and other non-alphanumerics) is `KEY`, `KEYS`, `PASS`, `PWD`, `PIN`, `AUTH`, `SESSION`, `CERT`, `CERTS`, `DSN`, `SALT`, `OTP`, `JWT` or `SID`, ends in `KEY`/`KEYS`, starts with `AUTH` (but not `AUTHOR`/`AUTHORS`), or starts or ends with `PASS`, `SESSION` or `CERT`;
  - never masked: `PWD`, `OLDPWD`, `XDG_SESSION_TYPE`, `XDG_SESSION_CLASS`, `XDG_SESSION_DESKTOP`, `XDG_SESSION_ID`, `XDG_SESSION_PATH`, `DESKTOP_SESSION`, `DBUS_SESSION_BUS_ADDRESS`, `SESSION_MANAGER`, `SSH_AUTH_SOCK`, `GPG_AGENT_INFO`. Words, not substrings, keep names such as `KEYBOARD` or `XKB_DEFAULT_KEYMAP` plain.
- **Another user's process** shows "Not readable (permission denied)": the kernel lets only the process's own user, or root / `CAP_SYS_PTRACE`, read it. A process that has exited shows "Process exited". Until TaskSmack has confirmed which process holds the PID (its start time), the section shows "Not available yet" and reads nothing; it tries again at the next 3-second refresh.
- Control characters and invalid UTF-8 in names and values are shown escaped (`\n`, `\xFF`).
- TaskSmack never writes environment values to its log.
- **Windows:** not available (reading another process's environment there needs its memory); the section is hidden.

### Connections

On Linux, the Process Details **Overview** tab also has a collapsible **Connections** section, closed by default, under the Environment section. Open it to list the selected process's TCP and UDP sockets, netstat-style, in a **PROTO / LOCAL ADDRESS / REMOTE ADDRESS / STATE** table with a count above it ("3 connections"):

- **PROTO** is `TCP`, `TCP6`, `UDP` or `UDP6` (IPv4 or IPv6).
- **Addresses** are shown as `address:port`: IPv4 dotted (`10.0.0.5:55026`), IPv6 in brackets and in its short form (`[2001:db8::1]:443`, `[::]:22`), with an IPv4 peer of an IPv6 socket as `[::ffff:192.0.2.1]:443`. A port of `*` means none: a listening or unconnected socket's remote end (`0.0.0.0:*`).
- **STATE** is the TCP state as netstat names it: `ESTABLISHED`, `LISTEN`, `SYN_SENT`, `SYN_RECV`, `FIN_WAIT1`, `FIN_WAIT2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, `TIME_WAIT`, `CLOSE`. A UDP socket that is only bound shows `UNCONN`; one that called `connect()` shows `ESTABLISHED`.
- Rows are sorted by state (established first, then listeners, unconnected UDP sockets and the closing states), then by remote address. Click a column header to sort by that column; click it again to reverse. Addresses sort numerically (`10.0.0.9` before `10.0.0.10`), IPv4 before IPv6. The sort you choose stays when you select another process.
- An address too long for its column is cut off at the column edge; hover it to see it whole. More than 12 rows scroll inside the section.
- TaskSmack reads the sockets only for the selected process and only while the section is open: once when you open it or select another process, then every 2 seconds, in the background, so the window never waits on a read. It matches the process's open socket descriptors (`/proc/[pid]/fd`) against the system's TCP and UDP sockets, read over netlink (`INET_DIAG`), or from `/proc/[pid]/net/tcp`, `tcp6`, `udp` and `udp6` where netlink is unavailable or the process is in another network namespace (a container).
- A connection in `TIME_WAIT` usually does not show: once a process closes a socket, the kernel keeps the `TIME_WAIT` entry but no process holds it any more. The same goes for other sockets no process holds.
- **Another user's process** shows "Not permitted (another user's process)": listing a process's socket descriptors needs the same rights as debugging it (its own user, or root / `CAP_SYS_PTRACE`). A process that has exited shows "Process exited", and until TaskSmack has confirmed which process holds the PID (its start time), "Not available yet". A process with no TCP or UDP sockets shows "No TCP or UDP sockets". If a socket table cannot be read, the section shows "Could not be read" with the reason rather than a partial list.
- **Windows:** not available yet (tracked in #1489); the section is hidden.

### Modules

The Process Details **Overview** tab also has a collapsible **Modules** section, closed by default, under the Connections section (on Windows, where Environment and Connections are hidden, it is the only one). Open it to list the code the selected process has loaded, its executable and every DLL or shared library, in a **NAME / VERSION / BASE / SIZE / PATH** table, with the count in the section's header ("Modules (87)"):

- **NAME** is the file name; **PATH** the full path. **BASE** is the address the module is loaded at, in hex (`0x7FF8A1B20000`), and **SIZE** its size in memory.
- **VERSION** is the file version from the module's version information (`10.0.26100.4202`), blank for a file without one. It is Windows only: Linux shared libraries carry no file version, so on Linux the column is left out.
- Rows are sorted by name. Click a column header to sort by that column; click it again to reverse. Type in the filter box to show only modules whose name or path contains the text (case doesn't matter). A path too long for its column is cut off at the column edge; hover it to see it whole. More than 12 rows scroll inside the section.
- TaskSmack reads the modules only for the selected process and only while the section is open: once when you open it or select another process, then every 3 seconds, in the background, so the window never waits on a read.
- **Windows:** listed with `EnumProcessModulesEx`, including a 32-bit process's 32-bit modules. Each module's version is read from its file once and remembered. A protected process, or an elevated one while TaskSmack is not running as administrator, shows "Access denied: a protected or elevated process, or another user's". A process that is still starting may briefly show "Could not be read" with the reason; the next refresh tries again.
- **Linux:** read from `/proc/[pid]/maps`. A module is a file with at least one executable mapping, so data files the process has mapped (a locale archive, fonts) are not listed, nor are anonymous memory and the kernel's `[heap]`, `[stack]` and `[vdso]`. Its base is its lowest mapping and its size the total of its mappings. A library that was deleted or replaced on disk after it was loaded (typically by a package upgrade) shows "(deleted)" after its name. Another user's process shows "Access denied ...": reading its mappings needs the process's own user, or root / `CAP_SYS_PTRACE`.
- A process that has exited shows "Process exited"; one whose start time TaskSmack does not know (Windows' Idle and System) shows "Not available for this process".

### Services

The **Services** tab lists the system's services, and on Windows starts, stops and restarts them and changes their startup type:

- **Columns:** **Name** (the service's short name), **Display name**, **State** (Running in the running colour, Starting/Stopping/Resuming/Pausing in the amber pending colour, Paused, Stopped muted), **Start type** (Automatic, Automatic (delayed), Manual, Disabled), **PID** (blank when the service isn't running) and **Account** (the account it logs on as).
- Hover a row for its description, its command line, its svchost group (for services that share an `svchost.exe`) and whether it runs in its own process or a shared one.
- Click a column header to sort by it; click again to reverse. Type in the filter box to show only services whose name or display name contains the text (case doesn't matter).
- The list is read only while the tab is shown, when you open it and then every 2 seconds, in the background. A service's configuration (start type, command line, account, description) is re-read every 30 seconds.
- **Windows:** read from the Service Control Manager without administrator rights. A service whose configuration Windows won't show to your account leaves those columns blank. If the Service Control Manager itself can't be opened, the tab says so (for example "Access to the Service Control Manager was denied") instead of showing an empty list. If a later read fails, the last list read stays on screen under an "Out of date: <reason>" line until a read succeeds again; if no read has ever succeeded, the tab shows "Couldn't read the services" with the reason.
- **Actions (Windows):** right-click a row for **Start**, **Stop**, **Restart** and **Startup type** (Automatic, Automatic (delayed), Manual, Disabled), or select a row and use the buttons above the table. An action that doesn't fit the service's state is greyed out (Start needs a stopped service, Stop a running or paused one, Restart a running one).
  - **Stop**, **Restart** and **Disabled** ask first, in a dialog centred over the window. Well-known services Windows depends on (for example RpcSs, EventLog, Winmgmt, LSM, CryptSvc, Dhcp and Dnscache) get a stronger warning.
  - The action runs in the background; a line above the table says "Stopping Spooler..." and then the outcome ("Stopped Spooler", or "Could not stop Spooler: Requires administrator"). Start, stop and restart wait up to 10 seconds for each change of state. The list is re-read straight after, start types included.
  - Stopping a service that other running services depend on is refused, naming them ("Stop the services that depend on it first: ..."): TaskSmack never stops dependents for you.
  - Most services need TaskSmack to run as administrator to be controlled. Without it, the actions stay available (a service's own permissions may allow some) and a note says "Requires administrator for most services"; a refused action reports "Requires administrator".
- **Linux:** not available yet ("Services aren't available on this platform yet"); systemd support, and control through it, is planned.

### Startup Apps

The **Startup** tab lists the programs Windows starts when you sign in, and enables or disables them:

- **Columns:** **Name**, **Publisher** (the company named in the program's version information), **Enabled** ("Enabled", or "Disabled since" the date it was disabled in Task Manager or Settings), **Scope** (Current user, or All users), **Location** (the Run or RunOnce registry key, including the 32-bit `WOW6432Node` view, or the user's or all-users Startup folder) and **Command** (the command line it runs; for a Startup folder shortcut, its target and arguments).
- An entry whose program no longer exists is shown in the warning colour; hover it to see the command and the missing program's path. A Startup folder shortcut whose target can't be read shows its shortcut path with "(target unresolved)".
- Click a column header to sort by it; click again to reverse. Type in the filter box to show only entries whose name, publisher or command contains the text (case doesn't matter).
- The list is read only while the tab is shown, when you open it and then every 5 seconds, in the background. Listing needs no administrator rights.
- **Enable / Disable (Windows):** right-click a row for **Enable** or **Disable**, or select a row and use the buttons above the table. TaskSmack does it the way Task Manager does: it records the entry's state under `Explorer\StartupApproved` and never deletes or moves the registry entry or the shortcut, so a disabled app can be enabled again here, in Task Manager or in Settings.
  - **Disable** asks first, in a dialog centred over the window; **Enable** runs straight away.
  - A line above the table says "Disabling OneDrive..." and then the outcome ("Disabled OneDrive", or "Could not disable OneDrive: Requires administrator"). The list is re-read straight after, so the **Enabled** column shows "Disabled since" today's date.
  - All-users entries (HKLM, and the all-users Startup folder) need TaskSmack to run as administrator: without it their actions are greyed out and say "Requires administrator". RunOnce entries run once at the next sign-in and have no enabled state, so they can't be enabled or disabled.
- Scheduled tasks that run at sign-in, and services, are not listed here.
- **Linux:** not available yet ("Startup apps aren't available on this platform yet"); XDG autostart support is planned.

### System Information

The **System** tab shows what this machine is, in titled sections of label/value rows (#1399). More sections (memory modules, disks, adapters, GPUs and displays, security) are planned.

- **Operating system:**
  - **Windows:** edition and version (for example "Windows 11 Home", "25H2"), build with its update revision, architecture, install date, boot time and uptime, computer name, workgroup or domain, user, locale, time zone with its current UTC offset, and the system and Windows directories.
  - **Linux:** distribution and version (`/etc/os-release`), kernel and architecture, init system, desktop and session type (`XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`), boot time and uptime, host name, user, locale, time zone, and whether TaskSmack is running in a container or a virtual machine.
- **Firmware & board:** the system's manufacturer, model, version, SKU and family; the BIOS vendor, version and release date; the firmware mode (UEFI or legacy BIOS); the SMBIOS version; the embedded controller's version, when there is one; the board's manufacturer, product and version; the chassis type (Desktop, Notebook, Convertible, ...) and manufacturer; and the platform role (desktop, mobile, server). The system serial number, UUID and board serial number are identifiers.
  - **Windows:** read from the SMBIOS table (`GetSystemFirmwareTable`), and the firmware mode from `GetFirmwareType`; no administrator rights needed.
  - **Linux:** read from `/sys/class/dmi/id`, UEFI when `/sys/firmware/efi` exists. The serial numbers, the UUID and the SMBIOS version are readable by root only on most systems, so they show "—" with "requires administrator" otherwise.
- **Read once:** the facts are read in the background the first time you open the tab, never while sampling. **Refresh** reads them again; the uptime is as of that read ("Read at" beside the button).
- **Unavailable values** show a muted "—"; hover it for the reason.
- **Identifiers hidden:** the user name, the computer name, a domain name, serial numbers and the system UUID are hidden until you tick **Show identifiers**, and are left out of copies until then.
- **Filter:** type to show only rows whose label or value contains the text, or every row of a section whose title does (case doesn't matter).
- **Copy:** each section's **Copy** button, or **Copy all**, puts the section(s) on the clipboard as plain `Label: Value` lines, ready for a bug report. Copies include every row (not just the filtered ones), less hidden identifiers.
- No administrator rights or network access are needed.

### Process Actions

Right-click any process row for Terminate, Kill, Stop and Resume. The **Actions** block of Process Details' **Overview** tab has those too, plus the priority controls; the row menu of a multi-selection can also set one priority for all the selected processes (see below):

| Action | Linux | Windows |
|--------|-------|---------|
| Terminate (SIGTERM / graceful) | ✅ | ✅ |
| Kill (SIGKILL / force) | ✅ | ✅ |
| Stop (SIGSTOP / suspend) | ✅ | ❌ |
| Resume (SIGCONT) | ✅ | ❌ |
| Change priority (nice): Process Details, or several selected processes from the row menu | ✅ | ✅ (mapped) |
| Change I/O priority (ionice class and level), Process Details only | ✅ | ❌ |
| Trace system calls (strace), Process Details only | ✅ (needs `strace` and a terminal emulator) | ❌ |

In Process Details the Overview's Identity, Runtime and Actions sections are bordered cards, each with its title inside, drawn like the cells of the CPU Cores grid. The Actions card sits beside the Identity and Runtime cards, no taller than they are, so the charts keep their height: a row with the Terminate and Kill buttons (and Suspend and Resume on Linux, followed by **Trace system calls (strace)**, which takes a row of its own when there is no room for it after them), then **Priority**, the priority slider under it and **Apply** under the slider (on Linux, then **I/O priority**, with its current setting after its **Apply**). The process's current priority is the Runtime card's **Priority** row and the **Priority** label's tooltip, not repeated in the Actions card. A result or error message appears under the last row. When the window is too narrow for three cards side by side, or the card needs more height than Identity and Runtime have (as the priority slider usually does), the Actions card moves onto its own row under them, at its full height. **F9** brings the Overview tab forward to show its Kill confirmation.

**The priority slider.** The highest priority is at the left, drawn in the theme's high-priority colour, and the lowest at the right; a badge over the thumb shows the value picked. On Linux it is the nice value, from -20 to 19: drag the thumb, or with the slider focused use Left/Right (one step), Page Up/Page Down (five), Home/End (either end) and 0 (the default). On Windows the slider has five stops, one per priority class: **High**, **Above Normal**, **Normal**, **Below Normal** and **Idle**, each named under its tick and on the badge. A drag snaps to the nearest stop; Left/Right (or Up/Down) move one class and Home/End go to either end. **Realtime** cannot be set from TaskSmack: a process that already has it shows "Realtime" on the badge, with a hollow thumb left of High, and can be moved down to a settable class. Apply is enabled once a class other than the process's own is picked. Nothing changes until **Apply** is pressed.

Destructive actions require confirmation. In Process Details, Terminate and Kill, which end the process, are drawn in red, apart from Suspend and Resume.

**Acting on several processes at once.** Right-click any row of a multi-selection (see "Selecting several processes" above) and the menu's actions read **Suspend 5 processes...**, **Terminate 5 processes...** and so on: they act on every selected process (Details and the Copy items still act on the row you right-clicked). Right-clicking a row outside the selection selects just that row, as before. One confirmation covers the whole batch: it says what the action does, lists the processes by name and PID (the first eight, then "and N more"), and always names TaskSmack itself and PID 1 (the init process) when either is among them, whatever the count, so neither can be acted on unnoticed. Each process is then acted on in turn, identified by its PID and start time exactly as a single action is, so one that has exited, or whose PID now belongs to another process, is refused rather than hit by mistake. TaskSmack's own process, if selected, is acted on last. The result is one line in the toolbar -- "Kill sent to 5 processes", or "Kill sent to 3 of 5 processes; 2 failed: ..." quoting the first few errors (when the line is cut short, hover it for the full text) -- rather than a message per process.

**Changing the priority of several processes at once.** Where the platform can set priority, the menu of a row in a multi-selection also has **Set priority for 5 processes...**. It opens a small dialog with the same control as Process Details: the nice slider on Linux (-20, the highest priority, to 19, the lowest; it starts at 0), on Windows the same slider with a stop per priority class (High, Above Normal, Normal, Below Normal, Idle; it starts at Normal). Under the control the dialog says what will be applied -- on Linux the label and the nice value, for example "Below Normal (nice: 10)"; on Windows the class, since Windows maps the value to a priority class. **Continue** goes on to the same batch confirmation as the actions above (the value to be applied, the processes, TaskSmack itself and PID 1 always named); **Set Priority** there sets it on each process in turn, by PID and start time, TaskSmack's own process last. **Cancel** in either dialog changes nothing. The result is one line in the toolbar naming what was applied: "Priority set to Below Normal (nice: 10) for 5 processes", or "... for 3 of 5 processes; 2 failed: ..." quoting the first few errors. On Linux, raising a process's priority (a nice value below 0) usually needs root, so without it expect those processes to refuse; the dialog and the confirmation both warn when the value is below 0. A single process's priority, and the I/O priority (Linux), are still set in the Actions block of Process Details' Overview tab.

#### I/O Priority (Linux)

On Linux, the Actions block of Process Details has an **I/O priority** control under the nice slider: one slider in the same style, through every `ionice` class and level, highest priority at the left. Hover the **I/O priority** label for the process's current setting, for example `best-effort 4`, `idle`, or `default (best-effort 4 from nice)`.

- **The scale**, in bands with a divider between them: **Realtime** 0 to 7 (served before everything else), **Best-effort** 0 to 7 (shares the disk by level), then **Idle** (gets the disk only when no other process wants it). Within a class, level 0 is the highest priority. The badge names the stop, for example `best-effort 4`.
- **Default.** A process whose I/O priority was never set follows its nice value: the kernel uses best-effort at level (nice + 20) / 5. The thumb then sits at that level, **hollow**. Picking any stop sets it explicitly; **Reset to default**, shown while the setting is explicit, goes back to following the nice value.
- **Realtime** is on the slider only when TaskSmack has `CAP_SYS_NICE` (or `CAP_SYS_ADMIN`), checked once at start. Without it the slider starts at best-effort 0, and a process already in Realtime is shown beyond its start, hollow, and can be lowered but not set back.
- **Keys**, with the slider focused: Left/Right or Up/Down one step (across the class bands), Home/End either end. Drag snaps to the nearest stop.
- **Apply** sets the class and level on every thread of the process. Like the nice control, it applies only to the process the edit was made for. Selecting another process discards an unapplied edit.

Lowering a process you own (Idle, or a higher best-effort level) needs no privilege. Two changes need `CAP_SYS_NICE` (or root): setting **Realtime**, and changing **another user's process**. Without that privilege the change is refused with an error saying so. Windows has no equivalent, so the control is not shown there.

#### Tracing system calls (Linux)

The Actions block of Process Details (on the Overview tab) has a **Trace system calls (strace)** button at the end of its button row (on a row of its own when the block is too narrow for it there), as htop's `s` key does: it opens a new terminal window running `strace -f -tt -p <PID>` attached to that process, so you can watch each system call it makes, timestamped, across all its threads. Close the window, or press Ctrl+C in it, to stop tracing; the process carries on running. No confirmation is asked, because tracing only observes (the process pauses for an instant while strace attaches, and runs slower while it is traced).

- TaskSmack looks for `strace` and a terminal once, when it starts, on `PATH` (relative `PATH` entries are ignored). The terminal is the one `$TERMINAL` names -- a single program name or absolute path, with no arguments -- else `x-terminal-emulator`, else the first installed of gnome-terminal, ptyxis, konsole, xfce4-terminal, mate-terminal, kitty, alacritty, foot, wezterm and xterm. If either is missing the button stays visible but greyed out, and its tooltip says what to install; restart TaskSmack after installing it. On Windows the button is not shown at all.
- The process is checked by PID and start time, like every other action, so one that has exited (or whose PID now belongs to another process) is refused.
- Attaching uses ptrace, which the kernel restricts. TaskSmack checks the usual restrictions first and explains them in the result line instead of opening a window that would close at once:
  - Without `CAP_SYS_PTRACE`, the kernel allows tracing only a process whose real, effective and saved user IDs and group IDs all match yours and that has not changed credentials. Anything else -- another user's process, or one that changed its user or group IDs -- needs `CAP_SYS_PTRACE`: TaskSmack running as root with that capability (root keeps it across exec), or a `strace` binary that carries it as a file capability. The capability is what counts, not EUID 0: root with `CAP_SYS_PTRACE` dropped (common in containers and hardened services) is limited like any other user, and TaskSmack treats root whose capabilities it cannot read the same way.
  - Yama's `/proc/sys/kernel/yama/ptrace_scope` -- 1 by default on Ubuntu -- lets a process attach only to its own descendants unless it has `CAP_SYS_PTRACE`. strace runs in a terminal TaskSmack starts, so it is never the traced process's ancestor: at 1 or 2 only a `strace` with `CAP_SYS_PTRACE` can trace, and at 3 nobody can. `sudo sysctl kernel.yama.ptrace_scope=0` relaxes this until the next reboot, for every program, so weigh that before doing it.
  - TaskSmack never asks for or raises privileges itself.
- strace and the terminal are started directly, never through a shell, so nothing in a process's name can be run as a command. The terminal inherits none of TaskSmack's open files.

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
| CPU Details: sockets, cores, cache sizes, base speed | ✅ (`/proc/cpuinfo`, `/sys/devices/system/cpu`) | ✅ |
| CPU Details: virtualization, hypervisor, VBS, memory integrity | ❌ (hidden) | ✅ (no elevated privileges needed) |
| Process I/O counters | ✅ (other users' processes: `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE`, which root has with its normal capabilities; root alone isn't enough where capabilities are dropped) | ✅ (no elevated privileges needed) |
| Per-process network (TCP only) | ✅ (Linux 4.2+ Netlink; other users' processes: `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE`, which root has with its normal capabilities; root alone isn't enough where capabilities are dropped) | ✅ (TCP EStats; administrator required) |
| Thread count per process | ✅ | ✅ |
| Process priority (nice) | ✅ | ✅ (mapped −20 … +19) |
| Process I/O priority (ionice class and level) | ✅ (Realtime and other users' processes: `CAP_SYS_NICE` or root) | ❌ (no equivalent) |
| Process terminate / kill | ✅ | ✅ |
| Process stop / resume (SIGSTOP/SIGCONT) | ✅ | ❌ |
| Trace system calls (Process Details) | ✅ (`strace` in a terminal; greyed out with the reason when strace or a terminal is missing; ptrace rules apply: own processes, or root / `CAP_SYS_PTRACE`, and Yama's `ptrace_scope`) | ❌ (button hidden) |
| I/O wait time (`iowait`) | ✅ (shown as its own band; counted as idle, not busy, so CPU % matches Windows) | ❌ (Windows concept does not exist) |
| Steal time (`steal`) | ✅ | ❌ |
| Load average (1/5/15 min) | ✅ | ❌ |
| Shared memory per process | ✅ (`/proc/[pid]/statm`) | ❌ |
| Process environment variables (Process Details) | ✅ (`/proc/[pid]/environ`, own user's processes, or root / `CAP_SYS_PTRACE`) | ❌ |
| Per-process TCP/UDP connections (Process Details) | ✅ (`INET_DIAG` or `/proc/[pid]/net/*`, own user's processes, or root / `CAP_SYS_PTRACE`) | ❌ (#1489) |
| Services tab | ❌ (planned: systemd) | ✅ (Service Control Manager; listing needs no administrator, most actions do) |
| System Information: Operating system section | ✅ (os-release, uname, `/proc`, XDG session, container/VM hints) | ✅ (CurrentVersion registry key, session APIs) |
| System Information: Firmware & board section | ✅ (`/sys/class/dmi/id`; serials, UUID and SMBIOS version need root) | ✅ (SMBIOS table, `GetFirmwareType`) |
| Startup apps tab | ❌ (planned: XDG autostart) | ✅ (Run/RunOnce keys, Startup folders; enable / disable through StartupApproved, all-users entries need administrator) |
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
- **Advanced:** buttons that open `config.toml` and the user themes folder, and **Show limited-data notice**, which turns the startup notice about missing administrator or root rights back on after its "Don't show again" was ticked, and **About TaskSmack**, which closes Settings (without saving) and opens About.

**Save** writes your changes to `config.toml` and closes the dialog; **Cancel** (or Escape) closes it without changing anything. **Reset to defaults** sets every control in the dialog back to its default, and Save keeps them.

Dialogs (Settings, About and the limited-data notice) and the Help window are kept inside the main window. When the font size or display scaling makes the Settings dialog taller than the window, its options scroll and the Cancel and Save buttons stay visible.

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

Older versions also wrote `[metrics] min_time_for_rate_seconds`, `[metrics] integrated_gpu_vram_threshold_mb`, `[ui] progress_color_low_threshold` and `[ui] progress_color_high_threshold`. None of them ever had an effect, and TaskSmack now removes them from `config.toml` the next time it saves. Network rates are measured over each interval, so no start-up delay is needed. The old configurable VRAM threshold is unused. On Windows, integrated and discrete GPUs are told apart by the driver's own report (DXCore); only where DXCore can't answer does TaskSmack guess from the vendor and the adapter's dedicated memory (an Intel GPU with under 512 MiB or an AMD GPU with under 1 GiB counts as integrated, a Qualcomm GPU always does, an NVIDIA GPU never does). On Linux they are told apart by PCI bus and, for AMD, graphics-core version. TaskSmack has no threshold-coloured progress bars.

### Running TaskSmack twice

Only one TaskSmack runs at a time for each config directory. Starting a second one shows "TaskSmack is already running", and the second one exits. Otherwise two copies saving the same `config.toml` could overwrite each other's settings. The check uses a `tasksmack.lock` file next to `config.toml`. The operating system releases the lock however TaskSmack exits, so you never need to delete the file. A Windows build and a WSL build on the same machine use different config directories, so both can run at once.
