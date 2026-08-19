# FAQ & Troubleshooting

---

## "Illegal instruction" error on launch

**Cause:** The binary was built with the `optimized` or `win-optimized` preset, which targets the x86-64-v3 microarchitecture (AVX2). Intel processors before Haswell (2013) and AMD processors before Excavator (2015) do not support these instructions.

**Fix:** Download the `release-compatible` (`win-release-compatible` on Windows) build from the [releases page](https://github.com/mgradwohl/tasksmack/releases/latest). It targets x86-64-v2 (2009+ CPUs) and runs on any modern x86-64 machine.

If you are building from source, use:

```bash
cmake --preset release-compatible   # Linux
cmake --preset win-release-compatible  # Windows
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

> On Windows, I/O counters are always available via `GetProcessIoCounters` — no elevated privileges are needed.

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

## Why does the network rate show averages instead of instantaneous values?

Per-process network rates (`sent bytes/s`, `received bytes/s`) are **lifetime averages**: total bytes transferred by that process since it was first observed, divided by elapsed wall-clock time.

This design avoids the instability of single-sample deltas for short-lived bursts, at the cost of responsiveness after behaviour changes. The averages converge toward instantaneous rates for long-running, steady-state connections.

System-wide and per-interface rates *are* computed as short-interval deltas and update at the configured refresh cadence.

---

## Why are per-process network rates missing?

TaskSmack hides per-process network data when the platform cannot attribute traffic.

- **Linux:** requires Linux 4.2 or later with Netlink `INET_DIAG` support.
- **Windows:** TCP EStats collection requires administrator privileges.

System-wide and per-interface throughput should still appear.

---

## Developer setup and `clangd` problems

Developer troubleshooting is maintained in the canonical [contributor guide](../../CONTRIBUTING.md), including prerequisite checks, LLVM setup, CMake presets, and `compile_commands.json`.
