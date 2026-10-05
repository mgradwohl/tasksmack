# FAQ & Troubleshooting

---

## "Illegal instruction" error on launch

**Cause:** You built TaskSmack yourself with the `optimized` or `win-optimized` preset, which targets the x86-64-v3 microarchitecture (AVX2). Intel processors before Haswell (2013) and AMD processors before Excavator (2015) do not support these instructions. The packages published on the [releases page](https://github.com/mgradwohl/tasksmack/releases/latest) use the `release`/`win-release` preset instead (default compiler optimizations, no AVX2 requirement), so this should not happen with an official download — if it does, please [file a bug](https://github.com/mgradwohl/tasksmack/issues/new).

**Fix:** Rebuild with a preset that doesn't require AVX2:

```bash
cmake --preset release-compatible   # Linux, x86-64-v2 (2009+ CPUs)
cmake --preset win-release-compatible  # Windows, x86-64-v2 (2009+ CPUs)
# or the plain "release"/"win-release" presets, which use default optimizations
```

---

## "Process I/O shows dashes" on Linux

**Cause:** Per-process I/O counters come from `/proc/[pid]/io`, which is readable only by the process owner or root.

**Fix (option 1 — run as root):**

```bash
sudo ./TaskSmack
```

**Fix (option 2 — grant capability):**

```bash
sudo setcap cap_dac_read_search+ep /path/to/TaskSmack
```

`CAP_DAC_READ_SEARCH` grants access to `/proc/[pid]/io` without requiring full root. Re-apply the capability after each update.

> On Windows, I/O counters are always available — they come from the bulk `SystemProcessInformation` snapshot, so no elevated privileges are needed.

---

## GPU section doesn't appear

**Cause:** No available backend discovered a usable GPU. TaskSmack combines operating-system APIs with optional vendor libraries and hides GPU sections when none can provide data.

| Vendor | Backend |
|--------|---------|
| NVIDIA | NVML from the installed NVIDIA driver; Windows can also expose fallback data through DXGI/PDH |
| AMD | ROCm SMI on Linux; DXGI/PDH capability-dependent data on Windows |
| Intel/generic | DRM/sysfs on Linux; DXGI/PDH on Windows |

**Checklist:**

1. Confirm the driver is installed: run `nvidia-smi` for NVIDIA, or `rocm-smi` for AMD on Linux with ROCm.
2. For NVML or ROCm, check that the shared library is discoverable through the normal loader path.
3. Restart TaskSmack after installing the driver.

---

## Load average is missing on Windows

**This is expected.** Load average is a Unix-specific metric derived from the kernel's run-queue length. Windows has no equivalent and does not expose this value. The load average row is hidden automatically on Windows.

---

## Why is GPU% greater than 100 %?

**Cause:** Per-process GPU utilisation is summed across all GPUs in the system. A process that actively uses two GPUs simultaneously can show GPU% up to `number_of_GPUs × 100 %`.

This matches how multi-CPU CPU% reporting works — it is intentional, not a bug.

---

## Where do I put custom themes?

Drop `.toml` theme files in the platform-specific user themes directory:

| Platform | Path |
|----------|------|
| Windows | `%APPDATA%\TaskSmack\themes\` |
| Linux | `~/.config/tasksmack/themes/` |

Create the folder if it does not exist, then restart TaskSmack. Your theme will appear in the theme selector in the settings menu.

See the built-in theme files (shipped alongside the application) for the expected key structure.

---

## How are per-process network rates measured?

Per-process network rates (`sent bytes/s`, `received bytes/s`) are the bytes transferred between two readings of the process's network counters, divided by the time between those readings. They rise while a transfer runs and fall back to zero when it stops.

On Linux the counters come from a socket statistics cache that is refreshed every `socket_stats_cache_ttl_ms` (500 ms by default), which can span several refreshes. Refreshes that reuse a cached reading keep showing the last rate, so a rate can take up to one cache lifetime to change after a transfer starts or stops. On Windows the counters are read every refresh.

Only TCP traffic is counted. The kernel (Linux) and TCP EStats (Windows) report byte counts per TCP connection, but not for UDP sockets. QUIC/HTTP3, WebRTC video calls, games, and DNS are therefore not attributed to any process. System-wide and per-interface rates come from the interface counters and include all traffic.

Each connection's own growth between two readings is credited to the process that owns it, so a connection closing doesn't erase the traffic on the others. A few bytes go uncounted:

- On Linux, a connection that is first seen before TaskSmack knows which process owns it is counted from the reading in which it is attributed. The socket-to-process map is rebuilt every 3 seconds.
- Bytes sent between a connection's last reading and its close are not counted.

On Linux, a socket shared by several processes, for example one inherited across `fork()`, is counted for the lowest PID.

---

## Why doesn't the network Total match the sum of the interfaces?

The Total counts hardware interfaces only. On Linux these are network cards, Wi-Fi, USB adapters, and Hyper-V or virtio NICs; bridges, `veth` pairs, VPN tunnels, VLANs, and bonds are left out. On Windows they are the adapters Windows reports as hardware; VPN adapters, Hyper-V and WSL `vEthernet` adapters, WAN Miniports, and tunnels such as Teredo are left out. Those interfaces carry traffic that also crosses a hardware interface, so counting them as well would double it. They are still listed and selectable on their own. When no hardware interface exists, as inside a container, every interface counts.

---

## Why are per-process network rates missing?

TaskSmack hides per-process network data when the platform cannot attribute traffic.

- **Linux:** requires Linux 4.2 or later with Netlink `INET_DIAG` support.
- **Windows:** TCP EStats collection requires administrator privileges.

System-wide and per-interface throughput should still appear.

---

## Why does the FPS in the status bar change?

TaskSmack only draws as many frames as the screen needs. The FPS readout in the bottom-right corner shows the rate it is drawing at, averaged over half a second:

- **Idle, about 20 FPS or less:** nothing on screen moves faster than half a pixel per frame at that rate. With the default 300-second history, the charts scroll only a few pixels a second.
- **Brief bursts, up to about 60 FPS:** a new sample has arrived and the now-bars or a chart's scale are easing to it, or a chart with a short history window scrolls quickly.
- **Moving the mouse or typing:** frames are capped at about 60 FPS. The cap is a whole number of display refreshes, never slower than 60 on a display refreshing at 60 Hz or faster, for example 60 at 60 or 120 Hz, 75 at 75 Hz, 72 at 144 Hz and 82.5 at 165 Hz, so motion stays even with vsync. A slower display caps at its own refresh rate (30 FPS on a 30 Hz display).
- **Minimized:** about 5 FPS. A window fully covered by other windows drops to the same rate only where the desktop reports it: on Wayland. X11 and Windows don't report an ordinarily covered window, so there it keeps its normal rate.

The readout shows the real frame time, so a stalled machine that draws 6 frames a second shows about 6 FPS.

---

## Developer setup and `clangd` problems

Developer troubleshooting is maintained in the canonical [contributor guide](../../CONTRIBUTING.md), including prerequisite checks, LLVM setup, CMake presets, and `compile_commands.json`.
