# FAQ & Troubleshooting

---

## "Illegal instruction" error on launch

**Cause:** You built TaskSmack yourself with the `optimized` or `win-optimized` preset, which targets the x86-64-v3 microarchitecture (AVX2). Intel processors before Haswell (2013) and AMD processors before Excavator (2015) do not support these instructions. The packages published on the [releases page](https://github.com/mgradwohl/tasksmack/releases/latest) use the `release`/`win-release` preset instead (default compiler optimizations, no AVX2 requirement), so this should not happen with an official download — if it does, please [file a bug](https://github.com/mgradwohl/tasksmack/issues/new).

**Fix:** Rebuild with a preset that doesn't require AVX2:

```bash
cmake --preset release-compatible      # Linux, x86-64-v2: needs SSSE3, SSE4.1/4.2 and POPCNT
cmake --preset win-release-compatible  # Windows, x86-64-v2: needs SSSE3, SSE4.1/4.2 and POPCNT
cmake --preset release                 # Linux, baseline x86-64: runs on any x86-64 CPU
cmake --preset win-release             # Windows, baseline x86-64: runs on any x86-64 CPU
```

The `release-compatible` presets need an x86-64-v2 CPU: Intel Nehalem (first-generation Core i3/i5/i7) or later, or AMD Bulldozer (FX) or later, or a Jaguar-based chip. Older CPUs, including Intel Core 2 and AMD Athlon II / Phenom II, lack some of those instructions and crash the same way on a `release-compatible` build; use the plain `release` / `win-release` preset there. If a `release-compatible` build is the one that crashes, switch to the plain preset too.

---

## How do I check that a download is genuine?

Every release file (the `.deb`, `.tar.gz`, `.zip` and Windows installer for each architecture, the Windows build-inputs manifests and the source SBOM) can be checked against TaskSmack's release workflow in either of two ways. Releases after v0.9.0 carry a GitHub build-provenance attestation for each file; with the [GitHub CLI](https://cli.github.com/) installed and signed in, run:

```bash
gh attestation verify <asset> --repo mgradwohl/tasksmack
```

Every release also has a `SHA256SUMS` listing each file's SHA-256, signed with Sigstore in `SHA256SUMS.bundle` (up to v0.9.x, a `<asset>.bundle` beside each file instead). Download both next to your files, then check them with [cosign](https://github.com/sigstore/cosign), which needs no GitHub account:

```bash
cosign verify-blob \
  --bundle SHA256SUMS.bundle \
  --certificate-identity-regexp '^https://github\.com/mgradwohl/tasksmack/\.github/workflows/release\.yml@refs/tags/.+$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

If either check fails, don't use the file. See the user guide's [Verifying a Release](user-guide.md#verifying-a-release) for what each check proves and what to do next.

---

## "Process I/O, FDs or network show N/A" on Linux

**Cause:** Per-process I/O counters come from `/proc/[pid]/io`, and FD counts from `/proc/[pid]/fd`, which are readable only by the process owner or root. Network rates are attributed to a process through its `/proc/[pid]/fd` too. For other users' processes these values are shown as N/A (not 0), and the system FD and network totals leave them out.

**Fix (option 1 — run as root):**

```bash
sudo ./TaskSmack
```

Run this way, links, **Open folder**, and Settings' **Edit Config File** and **Open Themes Folder** still open as you, in your desktop session: TaskSmack starts the browser or file manager as the user sudo (or pkexec) names, not as root.

**Fix (option 2 — grant capabilities):**

```bash
sudo setcap cap_dac_read_search,cap_sys_ptrace,cap_kill,cap_sys_nice+ep /path/to/TaskSmack
```

Each value needs a different kernel check, so one capability alone doesn't restore all of them:

| Value | Kernel check for another user's process | Capability needed |
|-------|------------------------------------------|-------------------|
| FD count | Listing `/proc/[pid]/fd` (a directory only its owner can read) | `CAP_DAC_READ_SEARCH` |
| I/O | Opening `/proc/[pid]/io` (owner-only) **and** ptrace read access to the process | `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE` |
| Network | Reading the `/proc/[pid]/fd/*` links, which needs ptrace read access to the process | `CAP_DAC_READ_SEARCH` + `CAP_SYS_PTRACE` |
| End, suspend or resume another user's process | Sending it a signal | `CAP_KILL` |
| Raise a priority (nice below 0, the Realtime I/O class); another user's priority or CPU affinity | `setpriority`, `ioprio_set`, `sched_setaffinity` | `CAP_SYS_NICE` |

With `CAP_DAC_READ_SEARCH` alone, only FD counts come back. The last two only matter for acting on processes: without them those actions are refused, and the startup notice's **What does Administrator add?** table lists them. `CAP_SYS_PTRACE` lets TaskSmack inspect any process on the system, and `CAP_KILL` lets it end any process, so grant them only if you are comfortable with that; running as root (with its normal capabilities) is the alternative, but root alone isn't enough in a container or hardened service that drops capabilities — allow them there. Re-apply the capabilities after each update.

**Standard or Administrator:** TaskSmack always starts as Standard, running as you. While it isn't root, the startup notice says so and offers **Authenticate for this session...**: one administrator password, after which the helper below reads and acts for TaskSmack until TaskSmack closes, so no further prompts come. The window itself stays yours; its title bar (and the taskbar entry) then reads **TaskSmack (Administrator)**, and the status bar shows **Administrator** with a shield. **Continue as Standard** (or Escape) keeps today's behaviour, and a dismissed or refused password leaves TaskSmack Standard, with a note saying so. Without the helper (the `.tar.gz`, or a source build without the packages), the option is disabled and says to install the `.deb` or `.rpm`. **What does Administrator add?** lists what each session gets. **Don't show this again** hides the notice; TaskSmack still starts as Standard.

**Acting on another user's process without either:** with the `.deb` (or an `.rpm` you build yourself) installed, an action refused for want of permission (ending, suspending or resuming a process, or changing its priority, I/O priority or CPU affinity) offers **Authenticate and retry** beside its error. It asks for an administrator's password through polkit, then does that one action as the administrator. Likewise, Process Details' **Environment**, **Modules**, **Open files** and **Connections** sections offer **Authenticate to read** when another user's process refuses it. For a few minutes afterwards, refused actions and reads are retried that way without asking again, and in that time PSS and USS on the Memory section also show for other users' processes (otherwise they read "not readable"), as do the Processes table's **Handles/FDs**, **I/O Read**, **I/O Write**, **Net Sent** and **Net Received** and System Information's root-only facts: the serial numbers, the UUID, the SMBIOS version, the memory modules and every user's crashes. Nothing else runs with elevated rights, and the portable `.tar.gz` doesn't include this. While polkit remembers the password (a few minutes), another program you run in the same session could also start TaskSmack's helper without a prompt, as with any action polkit keeps authorized. TaskSmack running as root offers none of this, since it needs no helper.

For administrators: the helper is `/usr/libexec/tasksmack/tasksmack-helper`, started through `pkexec` for the polkit action `app.tasksmack.TaskSmack.helper` (defined in `/usr/share/polkit-1/actions/app.tasksmack.TaskSmack.helper.policy`: `auth_admin_keep` for an active local session, `auth_admin` otherwise). `pkexec` starts it as root, but once it has checked its caller it stops being root: it runs as `nobody` (UID and GID 65534, with no supplementary groups) and keeps only the capabilities its requests need (`CAP_KILL`, `CAP_SYS_NICE`, `CAP_SYS_PTRACE` and `CAP_DAC_READ_SEARCH`, with `no_new_privs` set) under a seccomp filter that refuses writing files, running programs, loading code, tracing processes and opening sockets, serves only the TaskSmack that started it, and exits when that TaskSmack closes it or 5 minutes after it started or last did an action (reads, which TaskSmack repeats on its own timers, don't keep it running). After **Authenticate for this session**, it runs until TaskSmack closes instead.

On Windows, see [Acting as an administrator on Windows](#acting-as-an-administrator-on-windows).

If `/proc` is mounted with `hidepid` (`noaccess`/`1`, `invisible`/`2` or `ptraceable`), other users' processes aren't listed: `noaccess` still shows their PIDs to `ls /proc` but refuses everything else about them. The notice says so. Membership of the mount's `gid=` group, or `CAP_SYS_PTRACE`, shows them. So does the helper while it runs (after **Authenticate for this session**, or for a few minutes after any authenticated action or read): it lists the hidden processes and reads a fixed set of their `/proc` files (`stat`, `statm`, `status`, `cmdline`, `io` and `cgroup`), which TaskSmack shows as it shows its own.

> On Windows, I/O counters and handle counts are always available — they come from the bulk `SystemProcessInformation` snapshot, so no elevated privileges are needed. Network rates need administrator rights; without them every process's network rates read N/A (see *Why are per-process network rates missing?* below).

---

## Acting as an administrator on Windows

TaskSmack itself never runs elevated for this. An action Windows refuses for want of rights, such as
ending, suspending, re-prioritizing or re-pinning an elevated or another account's process, offers **Retry as
administrator** beside its error. If your account is an administrator, Process Details' **Modules**,
**Open files** and **Connections** sections also offer **Read as administrator** when such a process
refuses to be read. Either button starts `TaskSmackHelper.exe`, TaskSmack's administrator helper,
through Windows' own UAC prompt, in front of TaskSmack's window:

- **If your account is an administrator,** the prompt asks you to approve. For a few minutes afterwards,
  refused actions and reads are retried through the helper without asking again. The helper exits when
  TaskSmack does, or after 5 minutes without an action. An I/O priority change, which Windows doesn't
  support, doesn't keep it running.
- **If your account is a standard user,** only **Retry as administrator** is offered. The prompt asks for
  an administrator's name and password, and the helper then runs as that administrator. It does that one
  action and exits, so the next one asks again. It wouldn't read another account's process for you,
  which is why no **Read as administrator** is offered.
- **With Windows 11's Administrator Protection on,** every elevation runs as a separate,
  system-managed administrator account, never as you. So even an administrator gets the standard user's
  session: one action per prompt, and reads of your own processes only. **Read as administrator** is
  still offered once, since only the helper can tell for sure: the first read then says "Administrator
  Protection runs the administrator helper as a separate administrator account, never as yours, so it
  can't read other users' processes for you", and the button isn't offered again until TaskSmack is
  restarted.
- **If an administrator's prompt asks for a password and is answered with another administrator's
  account,** the same happens to that helper: its first read says "The administrator account that
  approved the prompt isn't yours", and **Read as administrator** isn't offered again until another
  prompt, such as one for **Retry as administrator**, starts a new helper. That prompt is answered
  afresh: answered with your own account, the button is back.

The prompt is waited for two minutes. If it isn't answered by then, the action fails with "the
administrator's consent wasn't given in time", and consent given after that isn't acted on: the helper it
starts exits at once, and the action has to be chosen again. Windows doesn't let TaskSmack take the
prompt down, so until it is answered a retry says the prompt is still up.

Nothing else runs with elevated rights. The helper never ends a critical system process (one whose end
would stop Windows), and it acts only on the process you chose, checked by its start time as well as
its PID.

**Where TaskSmack must be installed.** The helper works only for a TaskSmack installed where only an
administrator can change it, such as `C:\Program Files\TaskSmack`, where the installer
(`TaskSmack-<version>-win64.exe` or `-windows-arm64.exe`, from v0.9.1) puts it. That covers `TaskSmack.exe`,
`TaskSmackHelper.exe`, their folder, and every folder above it. If your account could replace either
file or move the folder, any program you run could put itself in TaskSmack's place, and the helper's
signed UAC prompt would then hand it administrator rights. So a portable copy unpacked from the `.zip`
into your Downloads, Desktop or another folder you own, or an install under your user profile, can't use
the helper: TaskSmack then doesn't offer the buttons. Everything else works as before.

The helper ships only in the Authenticode-signed installers and `.zip`s, x64 and ARM64, from v0.9.1
([#2302](https://github.com/mgradwohl/tasksmack/issues/2302)), so that its UAC prompt names TaskSmack's
publisher, **Matt Gradwohl**. No earlier release includes it, nor does any unsigned build; without
it, TaskSmack offers neither button. If TaskSmack is already running as
administrator, it needs no helper and offers neither.

---

## GPU section doesn't appear

**Cause:** No available backend discovered a usable GPU. TaskSmack combines operating-system APIs with optional vendor libraries (see the user guide's [GPU Monitoring](user-guide.md#gpu-monitoring) table) and hides GPU sections when none can provide data.

**Checklist:**

1. Confirm the driver is installed: run `nvidia-smi` for NVIDIA, or `rocm-smi` for AMD on Linux with ROCm.
2. For NVML or ROCm, check that the shared library is discoverable through the normal loader path.
3. Restart TaskSmack after installing the driver.

---

## Load average is missing on Windows

**This is expected.** Load average is a Unix-specific metric derived from the kernel's run-queue length. Windows has no equivalent and does not expose this value. The load average row is hidden automatically on Windows.

---

## How are a process's GPU% and GPU memory counted?

They are counted the way the GPU tab counts each GPU, so the two can be compared:

- **GPU %** is the process's utilization of the busiest GPU it uses, from 0 to 100 %. It is not summed across GPUs, so it never goes past 100 %.
- **GPU Mem** counts, on each GPU, the memory the GPU tab reports as used there: dedicated memory (VRAM) on a discrete GPU, shared system memory on an integrated GPU on Windows, even while the process uses none of it. It is added up across the GPUs the process uses, so it is never more than those GPUs show in use. Process Details also lists the dedicated and shared amounts separately when the process has shared GPU memory, which Windows reports and Linux does not.

---

## Where do I put custom themes?

In the `themes` folder of TaskSmack's config directory: `%APPDATA%\TaskSmack\themes\` on Windows, `$XDG_CONFIG_HOME/tasksmack/themes/` (usually `~/.config/tasksmack/themes/`) on Linux. Settings' (F2) **Open Themes Folder** opens it. Restart TaskSmack and the theme appears in the **Theme** list in Settings. See the user guide's [Themes and Configuration](user-guide.md#themes-and-configuration) for the steps, and the [theme reference](https://github.com/mgradwohl/tasksmack/blob/main/assets/themes/README.md) for every key.

---

## How are per-process network rates measured?

Per-process network rates (`sent bytes/s`, `received bytes/s`) are the bytes transferred between two readings of the process's network counters, divided by the time between those readings. They rise while a transfer runs and fall back to zero when it stops.

On Linux the counters come from a socket statistics cache that is refreshed every `socket_stats_cache_ttl_ms` (500 ms by default), which can span several refreshes. Refreshes that reuse a cached reading keep showing the last rate, so a rate can take up to one cache lifetime to change after a transfer starts or stops. On Windows the counters are read every refresh.

By default only TCP traffic is counted. The kernel (Linux) and TCP EStats (Windows) report byte counts per TCP connection, but not for UDP sockets. QUIC/HTTP3, WebRTC video calls, games, and DNS are therefore not attributed to any process. On Windows, turning on **Count UDP traffic per process** in Settings and running TaskSmack as administrator adds UDP, counted from the kernel's network events (see the user guide's "Counting UDP on Windows"). System-wide and per-interface rates come from the interface counters and include all traffic.

Each connection's own growth between two readings is credited to the process that owns it, so a connection closing doesn't erase the traffic on the others. A few bytes go uncounted:

- On Linux, the socket-to-process map is rebuilt every 3 seconds, and early (at most once a second) when a connection appears that it doesn't know, so a new connection is normally attributed in the reading it first appears in. When the early rebuild is held back, the bytes the connection moved up to that first reading are not counted; what it moves after that is credited to its process once it is attributed, as long as that happens within 8 seconds (`UNATTRIBUTED_SOCKET_HOLD_MS`: the 3-second map lifetime plus the 5-second longest refresh interval) of the connection first being seen. A connection that stays unattributed longer than that, such as one owned by a process TaskSmack can't read, stops holding: what it moved until then is dropped. If it is attributed later, the growth since the reading just before is counted, as for any connection; only when the attributing reading is itself the first one past the 8 seconds (after a suspend, say) is that interval dropped too, since it can't be split at the deadline. A connection that opens and closes before it is attributed is not counted.
- Bytes sent between a connection's last reading and its close are not counted.

On Linux, a socket shared by several processes, for example one inherited across `fork()`, is counted for the lowest PID.

---

## Why doesn't the network Total match the sum of the interfaces?

The Total counts hardware interfaces only. On Linux these are network cards, Wi-Fi, USB adapters, and Hyper-V or virtio NICs; bridges, `veth` pairs, VPN tunnels, VLANs, and bonds are left out. On Windows they are the adapters Windows reports as hardware; VPN adapters, Hyper-V and WSL `vEthernet` adapters, WAN Miniports, and tunnels such as Teredo are left out. Those interfaces carry traffic that also crosses a hardware interface, so counting them as well would double it. They are still listed and selectable on their own. When no hardware interface exists, as inside a container, every interface counts.

---

## Why are per-process network rates missing?

Per-process network rates read N/A when the platform cannot attribute traffic.

- **Linux:** requires Linux 4.2 or later with Netlink `INET_DIAG` support. A process in another network namespace than TaskSmack's (inside a container, say) reads N/A, since TaskSmack sees only its own namespace's sockets.
- **Windows:** TCP EStats collection requires administrator privileges.

System-wide and per-interface throughput should still appear.

---

## Why does the FPS in the status bar change?

TaskSmack only draws as many frames as the screen needs. The FPS readout, in the bottom-right corner of the status bar while the Render Metrics overlay is on (**Ctrl+Shift+M**), shows the rate it is drawing at, averaged over half a second:

- **Idle, about 20 FPS or less:** nothing on screen moves faster than that rate draws smoothly. With the default 300-second history, the charts scroll only a few pixels a second, and the small moves of the now-bars and chart scales on an idle machine ease at this rate too.
- **Brief bursts, up to about 60 FPS:** a new sample changed a value a lot, so a now-bar or a chart's scale moves quickly while it eases to it, or a chart with a short history window scrolls quickly.
- **Moving the mouse or typing:** frames are capped at about 60 FPS. The cap is a whole number of display refreshes, never slower than 60 on a display refreshing at 60 Hz or faster, for example 60 at 60 or 120 Hz, 75 at 75 Hz, 72 at 144 Hz and 82.5 at 165 Hz, so motion stays even with vsync. A slower display caps at its own refresh rate (30 FPS on a 30 Hz display).
- **Minimized:** no frames at all; TaskSmack draws again as soon as the window is restored. (On native Wayland, where the desktop doesn't say when a minimize ends, about 5 FPS instead.) A window fully covered by other windows stops drawing too, only where the desktop reports it: on Wayland. X11 and Windows don't report an ordinarily covered window, so there it keeps its normal rate. On Windows, a window on another virtual desktop, behind the lock screen or a disconnected remote session, or on a display that has turned off stops drawing as well, and draws at its normal rate again as soon as you can see it. The data is still sampled at the refresh interval you set, whatever the frame rate.

The readout shows the real frame time, so a stalled machine that draws 6 frames a second shows about 6 FPS.

---

## Developer setup and `clangd` problems

Developer troubleshooting is maintained in the canonical [contributor guide](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md), including prerequisite checks, LLVM setup, CMake presets, and `compile_commands.json`.
