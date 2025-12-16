# TaskSmack Architecture Overview (OpenGL + GLFW)

TaskSmack is a cross-platform system monitor and task manager that delivers a fast, ImGui-driven UI on top of accurate, high-frequency system metrics.

## Goals

- Immediate-mode UI built with Dear ImGui (docking + multi-viewport)
- OpenGL rendering routed through GLFW windowing
- Accurate metrics that stay responsive under load
- Strict separation between platform probes, data modeling, UI, and rendering
- Scalability to thousands of processes
- Extensibility without premature complexity

## Inspirations and References

- [Process Explorer (Sysinternals / Microsoft Learn)](https://learn.microsoft.com/en-us/sysinternals/downloads/process-explorer)
- [System Informer](https://www.systeminformer.com/)
- [System Informer GitHub](https://github.com/winsiderss/systeminformer)
- [System Explorer (TechSpot archive)](https://www.techspot.com/downloads/5015-system-explorer.html)
- [Glances (GitHub)](https://github.com/nicolargo/glances)
- [Glances Documentation](https://glances.readthedocs.io/en/latest/quickstart.html)
- [btop++ (GitHub)](https://github.com/aristocratos/btop)
- [iStat Menus (Bjango)](https://bjango.com/mac/istatmenus/)

## High-Level Architecture

```
[ Platform Probes ]  -->  [ Domain Snapshots ]  -->  [ UI Layers ]
        ^                         ^                       |
        |                         |                       v
     OS APIs               Data Modeling         OpenGL Renderer
                                                        |
                                                     GLFW
```

- Platform probes run on background threads, polling OS APIs and publishing raw counters.
- Domain code transforms counters into immutable snapshots and maintains history buffers.
- UI layers consume snapshots, render views through ImGui/ImPlot, and never call platform APIs directly.
- The renderer owns every OpenGL call; the rest of the application remains graphics-agnostic.

## Lessons from the Reference Architecture

### Core Library vs. Application Executable

- Core library owns reusable infrastructure (app lifecycle, windowing, rendering integration).
- Application executable stays thin, focusing on TaskSmack-specific logic and assets.
- **Takeaway:** keep TaskSmack logic lightweight; move reusable systems into libraries.

### Layer-Based Composition

- Layers expose `onAttach`, `onDetach`, `onUpdate(delta)`, `onRender`, `onEvent`.
- Layers update sequentially; events propagate top-down until handled.
- **Takeaway:** model TaskSmack panes (shell, processes, metrics) as layers managed by a stack.

### Window Owns OS Events, Application Routes Them

- GLFW receives native callbacks.
- Core translates them into internal events.
- Application dispatches events through the layer stack.
- **Takeaway:** isolate GLFW-specific code inside core; keep the rest of the system platform-agnostic.

### Rendering Isolated Behind a Backend

- Keep every OpenGL call in a renderer module to simplify ImGui integration and allow future backend swaps if needed.

## Repository Layout

```
TaskSmack/
  CMakeLists.txt
  cmake/
    Dependencies.cmake
    Toolchains/
  apps/
    tasksmack/
      CMakeLists.txt
      assets/
        fonts/
        icons/
      src/
        Main.cpp
        layers/
          ShellLayer.*
          ProcessLayer.*
          MetricsLayer.*
  libs/
    core/
    render/
    ui/
    domain/
    platform/
  third_party/
    imgui/
    implot/
    glad/
```

## Module Responsibilities

### libs/core

- Owns the application loop, layer stack, and event dispatch.
- Creates and manages the GLFW window (and thereby the OpenGL context).
- Provides time utilities, logging bootstrap, and shutdown coordination.
- Contains zero platform metrics code and only minimal OpenGL usage for context creation.

### libs/render

- Initializes the OpenGL context through GLFW and loads GL functions (glad or glbinding).
- Defines frame lifecycle helpers (`beginFrame()`, `endFrame()`) and exposes frame metadata (DPI, framebuffer size).
- Manages ImGui font/icon textures and shared GPU resources.
- Every OpenGL call lives here; UI and domain code never touch `gl*` symbols.

### libs/ui

- Configures Dear ImGui and ImPlot (contexts, styling, ini persistence).
- Hooks the ImGui GLFW and OpenGL3 backends.
- Hosts shared widgets, tables, and chart components.
- Consumes immutable domain snapshots plus renderer-provided frame info.
- Contains no direct OS or OpenGL calls outside the ImGui backends.

### libs/domain

- Defines immutable snapshot types representing system state.
- Implements history buffers with decimation (`History<T>` ring buffers).
- Owns `ProcessModel`, `SystemModel`, and rate calculations derived from counter deltas.
- Enforces a cross-platform metrics contract (CPU% semantics, PID reuse handling, delta-based rates).
- Deterministic and unit-testable; no GLFW, OpenGL, or OS calls.

### libs/platform

- Declares probe interfaces (`IProcessProbe`, `ISystemProbe`, `INetworkProbe`, `IDiskProbe`, ...).
- Provides platform implementations under `windows/` and `linux/`.
- Runs sampler threads on cadences such as 250 ms and 1 s, emitting raw counters from OS APIs.
- Produces raw measurements only; domain transforms them into UI-ready data.

## Dependency Direction

```
apps/tasksmack
    ↓
core → render → ui → domain
                    ↑
                 platform
                    ↑
                  OS APIs
```

Rules:
- Domain depends on nothing else.
- UI never calls platform APIs.
- Platform never depends on UI or renderer.
- OpenGL usage is confined to `libs/render`.

## UI Layer Model

- **ShellLayer:** docking root, main menu bar, global settings (refresh cadence, theme, column visibility), shared selection state.
- **ProcessLayer:** ImGui tables for process list with sorting/filtering/search plus detailed panes backed by cached process data.
- **MetricsLayer:** ImPlot timelines for CPU, memory, disk, and network histories using ring buffers and the latest snapshot.
- **OverlayLayer (optional):** modals, toast notifications, debug overlays without cluttering core layers.

## Sampling and Snapshot Pipeline

1. **Sampler threads (platform)** poll OS APIs on fixed intervals (250 ms, 500 ms, 1 s tiers) to capture process tables and counters.
2. **SnapshotBuilder (domain)** computes deltas, derives rates (CPU%, IO/s, Net/s), produces immutable snapshots keyed by PID + start time, and updates history buffers with decimation.
3. **UI thread** reads the latest snapshot atomically and renders without blocking on sampling, keeping the interface responsive under load.

## Process Scalability Guidance

- Maintain a stable cache keyed by PID + start time to cope with PID reuse.
- Build hierarchical process trees incrementally or during sampling, not every frame on the UI thread.
- Perform sorting and filtering off the UI thread and publish ready-to-render view models.

## OpenGL + GLFW Integration Details

- GLFW handles window creation, input, DPI, framebuffer scaling, and multi-viewport support.
- OpenGL core profile (3.3+) is recommended; only the renderer and ImGui backend issue GL calls.
- ImGui integrations: `imgui_impl_glfw` for events and `imgui_impl_opengl3` for rendering.
- GLFW callbacks feed the core event system before events reach the layer stack.

## Platform Strategy

### Windows
- Processes and CPU: `NtQuerySystemInformation` (SystemProcessInformation).
- Disk and network: begin with simpler APIs, plan for ETW kernel providers for fidelity.
- Services: SCM APIs and registry queries.
- GPU (optional): DXGI queries plus NVML for NVIDIA.

### Linux
- Processes and CPU: `/proc/stat`, `/proc/[pid]/stat`, `/proc/[pid]/io`, `/proc/meminfo`.
- Networking: `/proc/net/*` for a baseline; Netlink (`NETLINK_INET_DIAG`) for high-fidelity connections.
- Services: systemd D-Bus if parity with Windows services is desired.
- GPU: DRM/sysfs for temperatures, NVML for NVIDIA hardware.

Each platform implements the same probe interfaces; automated tests ensure contract compliance.

## Plugin Strategy (Deferred)

- Use a C ABI surface (`extern "C"`) with versioned function tables and plain structs.
- Support serialized payloads (msgpack/json) if ABI-neutral data exchange is required.
- Consider out-of-process plugins over IPC for high isolation.
- Avoid exposing a C++ ABI until a compelling use case appears.

## Security and Privacy Considerations

- **Least privilege:** request elevation only when required, provide clear prompts and audit logs.
- **Sensitive actions:** confirm disruptive operations (handle closing, driver/service edits).
- **Data handling:** disable telemetry by default; keep remote access opt-in and local-only unless reconfigured.
- **Hardening:** sandbox plugin execution, default-deny risky operations, plan for signed update channels.

## Feature Roadmap

1. **Foundation**
   - GLFW + ImGui docking shell
   - Metrics contract implementation and sampler threads
   - Basic CPU/memory metrics and process list on a single platform
2. **Core Monitoring**
   - Per-process CPU, memory, IO metrics with histories
   - Network interface stats and secondary platform support
3. **Controls and Polish**
   - Process controls (kill, priority adjustments)
   - GPU metrics (best effort per vendor)
   - Config file integration (toml++) and theming
4. **Advanced Features**
   - Services and startup managers
   - Plugin system enablement
   - Remote API (read-only first)
   - Handle/DLL inspection

## Recommended Stack Recap

- Windowing/rendering: GLFW + OpenGL + ImGui (docking) + ImPlot
- Concurrency: `std::jthread` with `std::stop_token`; coroutines optional later
- Data model: immutable snapshots with ring-buffer histories
- Configuration: toml++ (or JSON if preferred)
- Logging: spdlog
- Profiling: Tracy (optional but useful early)

This structure keeps TaskSmack UI-first, snapshot-driven, and cleanly layered, delivering a fast, accurate task manager while leaving room for future extensions.

## Future Features (btop++ Inspired)

Features observed in btop++ that could enhance TaskSmack. Organized by effort and priority.

### Medium Effort (Days)

| Feature | Description | Implementation Notes |
|---------|-------------|---------------------|
| **Network Panel** | Per-interface throughput (bytes/sec up/down), total bandwidth | New `INetworkProbe` reading `/proc/net/dev` (Linux), `GetIfTable2` (Windows) |
| **Disk Panel** | Per-device I/O rates, read/write activity | New `IDiskProbe` reading `/proc/diskstats` (Linux), `GetDiskPerformance` (Windows) |
| **Process Tree View** | Hierarchical view showing parent-child relationships | Already have `parentPid`; add collapsible tree rendering in UI |
| **Per-Process I/O Rates** | Read/write bytes per second for each process | Requires `/proc/[pid]/io` (may need elevated privileges on Linux) |
| **Column Visibility Toggles** | User-configurable visible columns with persistence | Add to config file and Settings dialog |

### Higher Effort (Weeks)

| Feature | Description | Implementation Notes |
|---------|-------------|---------------------|
| **Temperature Sensors** | CPU, GPU, NVMe temperatures | Linux: hwmon/sysfs, lm-sensors; Windows: WMI, vendor SDKs |
| **Power/Battery Stats** | Power consumption, battery state | Linux: `/sys/class/power_supply`; Windows: `GetSystemPowerStatus` |
| **GPU Stats** | GPU utilization, memory, temperature | NVML for NVIDIA, ROCm for AMD, vendor-specific |
| **Process Environment & Arguments** | Full command line and environment variables | `/proc/[pid]/cmdline`, `/proc/[pid]/environ` |
| **Signal Sending** | Send arbitrary signals to processes | Linux: `kill()`, Windows: `TerminateProcess`, `GenerateConsoleCtrlEvent` |
| **Priority Adjustment** | Change process nice/priority values | Linux: `setpriority()`, Windows: `SetPriorityClass` |

### UI Polish Features

| Feature | Description |
|---------|-------------|
| **Customizable Colors** | Color scheme customization beyond themes |
| **Mouse-Only Mode** | Full functionality without keyboard |
| **Keyboard Navigation** | Vim-style or arrow key process selection |
| **Mini-Mode/Compact View** | Condensed view showing key metrics only |
| **Panel Arrangement** | User-configurable panel layout and sizing |
| **Metric Alerts** | Visual/audio alerts when thresholds exceeded |

### Research Required

| Feature | Notes |
|---------|-------|
| **Firewall Rules** | Very platform-specific; may not be worth the complexity |
| **Container Awareness** | Docker/podman container grouping; requires container runtime APIs |
| **Service Management** | systemd on Linux, SCM on Windows; complex permissions |

## Future Features (htop Inspired)

Features observed in htop that could enhance TaskSmack.

### Quick Wins (Implemented)

| Feature | Status | Notes |
|---------|--------|-------|
| **MEM% Column** | ✅ Done | Memory as percentage of total system RAM |
| **TIME+ Column** | ✅ Done | CPU time formatted as H:MM:SS.cc |
| **Command Column** | ✅ Done | Full command line from `/proc/[pid]/cmdline` |
| **Task Summary** | ✅ Done | "N processes, M running" in panel header |
| **VIRT Column** | ✅ Done | Virtual memory size |

### Low Effort (Hours)

| Feature | Description | Implementation Notes |
|---------|-------------|---------------------|
| **NI (Nice) Column** | Display process nice value (-20 to 19) | Already captured in probe; add column to ProcessesPanel |
| **SHR Column** | Shared memory size | Linux: field from `/proc/[pid]/statm`; Windows: working set counters |
| **State Color Coding** | Color-code process states (R=green, S=gray, D=yellow, Z=red) | Add ImGui color push/pop in state column rendering |
| **Thread Count Column** | Number of threads per process | Already in ProcessCounters where supported |

### Medium Effort (Days)

| Feature | Description | Implementation Notes |
|---------|-------------|---------------------|
| **Process Tree View (F5)** | Collapsible parent-child hierarchy | Use `parentPid` to build tree; indent children in table |
| **Incremental Search** | Filter processes as you type without search box focus | Global keyboard input handling in panel |
| **Sort Indicator** | Visual arrow showing sort column and direction | ImGui table sorting specs already provide this |
| **Process Details Panel** | Expanded view showing environment, open files, connections | Modal or split panel; multiple `/proc/[pid]/*` reads |

### Higher Effort (Weeks)

| Feature | Description | Implementation Notes |
|---------|-------------|---------------------|
| **I/O Tab/View** | Dedicated I/O view showing read/write rates | Requires elevated privileges on Linux for `/proc/[pid]/io` |
| **Keyboard Shortcuts** | F-key shortcuts (F1=Help, F2=Setup, F5=Tree, F9=Kill, F10=Quit) | Global key handler in ShellLayer |
| **Setup Dialog (F2)** | Configure columns, colors, meters, layout | Persist to config file; apply changes live |
| **Help Screen (F1)** | Built-in help overlay explaining columns and shortcuts | Modal window with keyboard shortcut reference |
| **Strace Integration** | Attach strace to selected process | Linux only; spawn `strace -p <pid>` in terminal |
| **Lsof Integration** | Show open files for selected process | Linux: spawn `lsof -p <pid>`; Windows: Handle enumeration |


