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

## Future Features

This section consolidates all feature ideas from various sources, organized by effort level and value. Each feature includes its source reference.

### Quick Wins (Hours - High Value)

| Feature | Description | Source | Implementation Notes |
|---------|-------------|--------|---------------------|
| **Column Tooltips** | Show column descriptions on header hover | btop++ | Use ImGui tooltip API; Tracked in [#152](https://github.com/mgradwohl/tasksmack/issues/152) |
| **Sort Indicator** | Visual arrow showing sort column and direction | htop | ImGui table sorting specs already provide this; Tracked in [#153](https://github.com/mgradwohl/tasksmack/issues/153) |
| **Start Time Column** | When the process started | Task Manager | Already in probe (`startTimeTicks`), need UI column; Tracked in [#154](https://github.com/mgradwohl/tasksmack/issues/154) |
| **Incremental Search** | Filter processes as you type without search box focus | htop | Global keyboard input handling in panel; Tracked in [#155](https://github.com/mgradwohl/tasksmack/issues/155) |
| **EditorConfig** | Ensures consistent editor settings (indentation, line endings, charset) across all editors | Development | 5 min - prevents CRLF/LF diffs; Tracked in [#129](https://github.com/mgradwohl/tasksmack/issues/129) |
| **Hardening flags** | Security compiler flags (`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-fPIE`, CFI) | Development | 15 min - protect against buffer overflows and ROP attacks; Tracked in [#134](https://github.com/mgradwohl/tasksmack/issues/134) |
| **Pre-commit hooks** | Automatically run clang-format and other checks before each commit | Development | 30 min - catch issues before CI; Tracked in [#135](https://github.com/mgradwohl/tasksmack/issues/135) |
| **GitHub Release workflow** | Automatically creates releases with pre-built binaries when you push a version tag | Development | 1 hr - streamlines release process; Tracked in [#54](https://github.com/mgradwohl/tasksmack/issues/54) |
| **Changelog generation** | Conventional commit messages combined with automated changelog generation | Development | 1 hr - professional release notes via git-cliff; Tracked in [#98](https://github.com/mgradwohl/tasksmack/issues/98) |
| **`std::expected` examples** | Modern, type-safe alternative to exceptions or error codes for recoverable errors | Development | 1 hr - idiomatic C++23 error handling; Tracked in [#137](https://github.com/mgradwohl/tasksmack/issues/137) |

### Low Effort (Days - Medium Value)

| Feature | Description | Source | Implementation Notes |
|---------|-------------|--------|---------------------|
| **Process Tree View** | Hierarchical view showing parent-child relationships | btop++, htop | Already have `parentPid`; add collapsible tree rendering in UI; Tracked in [#158](https://github.com/mgradwohl/tasksmack/issues/158) |
| **Process Details Panel** | Shows detailed process information (environment, open files, connections) | htop | Currently partial - missing environment, open files, connections; Tracked in [#159](https://github.com/mgradwohl/tasksmack/issues/159) |
| **Keyboard Navigation** | Vim-style or arrow key process selection | btop++ | Enhance UI input handling; Tracked in [#160](https://github.com/mgradwohl/tasksmack/issues/160) |
| **Mouse-Only Mode** | Full functionality without keyboard | btop++ | Ensure all features accessible via mouse; Tracked in [#161](https://github.com/mgradwohl/tasksmack/issues/161) |
| **Mini-Mode/Compact View** | Condensed view showing key metrics only | btop++ | Alternative layout preset; Tracked in [#162](https://github.com/mgradwohl/tasksmack/issues/162) |
| **Panel Arrangement** | User-configurable panel layout and sizing | btop++ | Save/restore layout to config; Tracked in [#163](https://github.com/mgradwohl/tasksmack/issues/163) |
| **Customizable Colors** | Color scheme customization beyond themes | btop++ | Extend theming system; Tracked in [#164](https://github.com/mgradwohl/tasksmack/issues/164) |
| **Dev container** | `.devcontainer/` configuration enables instant development environments | Development | 2-3 hrs - VS Code and GitHub Codespaces support; Tracked in [#136](https://github.com/mgradwohl/tasksmack/issues/136) |
| **Benchmark framework** | Google Benchmark integration for performance measurement | Development | 1-2 hrs - track performance regressions; Tracked in [#104](https://github.com/mgradwohl/tasksmack/issues/104) |
| **Compile-time metrics** | Use `-ftime-trace` to identify slow headers | Development | Built into Clang; Tracked in [#141](https://github.com/mgradwohl/tasksmack/issues/141) |
| **Unity builds** | `CMAKE_UNITY_BUILD` for faster full rebuilds | Development | Single CMake variable; Tracked in [#142](https://github.com/mgradwohl/tasksmack/issues/142) |
| **`std::mdspan` examples** | Multi-dimensional array views for scientific computing | Development | Fully supported in Clang 22; Tracked in [#143](https://github.com/mgradwohl/tasksmack/issues/143) |
| **`std::ranges` pipelines** | Modern iteration patterns with range adaptors | Development | Fully supported in Clang 22; Tracked in [#144](https://github.com/mgradwohl/tasksmack/issues/144) |
| **License scanning** | REUSE compliance for clear license information | Development | REUSE tool via pip; Tracked in [#150](https://github.com/mgradwohl/tasksmack/issues/150) |

### Medium Effort (Weeks - High Value)

| Feature | Description | Source | Implementation Notes |
|---------|-------------|--------|---------------------|
| **Network Panel** | Per-interface throughput (bytes/sec up/down), total bandwidth | btop++ | New `INetworkProbe` reading `/proc/net/dev` (Linux), `GetIfTable2` (Windows); Tracked in [#167](https://github.com/mgradwohl/tasksmack/issues/167) |
| **Disk Panel** | Per-device I/O rates, read/write activity | btop++ | New `IDiskProbe` reading `/proc/diskstats` (Linux), `GetDiskPerformance` (Windows); Tracked in [#168](https://github.com/mgradwohl/tasksmack/issues/168) |
| **Per-Process I/O Rates** | Read/write bytes per second for each process | btop++, Task Manager | Requires `/proc/[pid]/io` (elevated privileges on Linux), `GetProcessIoCounters` (Windows); Tracked in [#169](https://github.com/mgradwohl/tasksmack/issues/169) |
| **I/O Tab/View** | Dedicated I/O view showing read/write rates | htop | Requires elevated privileges on Linux for `/proc/[pid]/io`; Tracked in [#202](https://github.com/mgradwohl/tasksmack/issues/202) |
| **Keyboard Shortcuts** | F-key shortcuts (F1=Help, F2=Setup, F5=Tree, F9=Kill, F10=Quit) | htop | Global key handler in ShellLayer; Tracked in [#170](https://github.com/mgradwohl/tasksmack/issues/170) |
| **Setup Dialog (F2)** | Configure columns, colors, meters, layout | htop | Persist to config file; apply changes live; Tracked in [#171](https://github.com/mgradwohl/tasksmack/issues/171) |
| **Help Screen (F1)** | Built-in help overlay explaining columns and shortcuts | htop | Modal window with keyboard shortcut reference; Tracked in [#172](https://github.com/mgradwohl/tasksmack/issues/172) |
| **Metric Alerts** | Visual/audio alerts when thresholds exceeded | btop++ | Configurable threshold monitoring; Tracked in [#173](https://github.com/mgradwohl/tasksmack/issues/173) |
| **FreeType Font Rendering** | Sharper text rendering at small sizes using FreeType | btop++ | Better hinting control; adds FreeType dependency; Tracked in [#174](https://github.com/mgradwohl/tasksmack/issues/174) |
| **Unified Settings File** | Consolidate ImGui's INI persistence into TOML config | btop++ | Set `io.IniFilename = nullptr`, embed ImGui state in TOML; Tracked in [#175](https://github.com/mgradwohl/tasksmack/issues/175) |
| **Fuzzing** | libFuzzer integration for automated bug finding | Development | 2 hrs - coverage-guided fuzzing; Tracked in [#102](https://github.com/mgradwohl/tasksmack/issues/102) |
| **Property-based testing** | rapidcheck for declarative test properties | Development | Available via FetchContent; Tracked in [#139](https://github.com/mgradwohl/tasksmack/issues/139) |
| **Dependency scanning** | `osv-scanner` for scanning C++ dependencies for vulnerabilities | Development | Standalone binary; Tracked in [#147](https://github.com/mgradwohl/tasksmack/issues/147) |
| **Include-what-you-use** | More reliable than clang-tidy's `misc-include-cleaner` | Development | Separate tool from LLVM; Tracked in [#148](https://github.com/mgradwohl/tasksmack/issues/148) |
| **SBOM generation** | Software Bill of Materials for supply chain security compliance | Development | Tools: syft, cyclonedx-cli; Tracked in [#149](https://github.com/mgradwohl/tasksmack/issues/149) |
| **Cross-compilation presets** | CMake presets for ARM64 and WebAssembly targets | Development | ARM64: needs cross-compiler; WASM: Emscripten SDK; Tracked in [#151](https://github.com/mgradwohl/tasksmack/issues/151) |

### Higher Effort (Months - High Value)

| Feature | Description | Source | Implementation Notes |
|---------|-------------|--------|---------------------|
| **Temperature Sensors** | CPU, GPU, NVMe temperatures | btop++ | Linux: hwmon/sysfs, lm-sensors; Windows: WMI, vendor SDKs; Tracked in [#176](https://github.com/mgradwohl/tasksmack/issues/176) |
| **Power/Battery Stats** | Power consumption, battery state | btop++ | Linux: `/sys/class/power_supply`; Windows: `GetSystemPowerStatus`; Tracked in [#177](https://github.com/mgradwohl/tasksmack/issues/177) |
| **GPU Stats** | GPU utilization, memory, temperature | btop++, Task Manager | NVML for NVIDIA, ROCm for AMD, D3DKMT (Windows), DRM (Linux); Tracked in [#178](https://github.com/mgradwohl/tasksmack/issues/178) |
| **Process Environment & Arguments** | Full command line and environment variables | btop++ | `/proc/[pid]/cmdline`, `/proc/[pid]/environ`; Tracked in [#179](https://github.com/mgradwohl/tasksmack/issues/179) |
| **Signal Sending** | Send arbitrary signals to processes | btop++ | Linux: `kill()`, Windows: `TerminateProcess`, `GenerateConsoleCtrlEvent`; Tracked in [#180](https://github.com/mgradwohl/tasksmack/issues/180) |
| **Priority Adjustment** | Change process nice/priority values | btop++ | Linux: `setpriority()`, Windows: `SetPriorityClass`; Tracked in [#181](https://github.com/mgradwohl/tasksmack/issues/181) |
| **Strace Integration** | Attach strace to selected process | htop | Linux only; spawn `strace -p <pid>` in terminal; Tracked in [#182](https://github.com/mgradwohl/tasksmack/issues/182) |
| **Lsof Integration** | Show open files for selected process | htop | Linux: spawn `lsof -p <pid>`; Windows: Handle enumeration; Tracked in [#183](https://github.com/mgradwohl/tasksmack/issues/183) |
| **Publisher Column** | Software publisher/vendor information | Task Manager | Windows: PE version info from `GetFileVersionInfo`; Linux: N/A; Tracked in [#184](https://github.com/mgradwohl/tasksmack/issues/184) |
| **Type Column** | Process type (App, Background, Windows process) | Task Manager | Windows-specific classification; Tracked in [#185](https://github.com/mgradwohl/tasksmack/issues/185) |
| **Status Column** | Suspended, Efficiency mode, etc. | Task Manager | Windows: `NtQueryInformationProcess`; Linux: cgroups; Tracked in [#186](https://github.com/mgradwohl/tasksmack/issues/186) |
| **Per-Process Disk I/O** | Disk I/O rate per process | Task Manager | Requires ETW (Windows) or eBPF (Linux); Tracked in [#187](https://github.com/mgradwohl/tasksmack/issues/187) |
| **Per-Process Network** | Network usage per process | Task Manager | Windows: ETW or `GetPerTcpConnectionEStats`; Linux: netstat/ss parsing; Tracked in [#188](https://github.com/mgradwohl/tasksmack/issues/188) |
| **GPU Engine Column** | Which GPU engine is in use | Task Manager | Very Windows/vendor specific; Tracked in [#189](https://github.com/mgradwohl/tasksmack/issues/189) |
| **Power Usage Column** | Process power consumption | Task Manager | Windows: `PROCESS_POWER_THROTTLING_STATE`; Linux: powercap; Tracked in [#190](https://github.com/mgradwohl/tasksmack/issues/190) |
| **Performance profiling** | Add perf/Instruments support, PGO builds | Development | Profile-guided optimization; Tracked in [#138](https://github.com/mgradwohl/tasksmack/issues/138) |

### Research Required (Value TBD)

| Feature | Source | Notes |
|---------|--------|-------|
| **Firewall Rules** | btop++ | Very platform-specific; may not be worth the complexity; Tracked in [#191](https://github.com/mgradwohl/tasksmack/issues/191) |
| **Container Awareness** | btop++ | Docker/podman container grouping; requires container runtime APIs; Tracked in [#192](https://github.com/mgradwohl/tasksmack/issues/192) |
| **Service Management** | btop++ | systemd on Linux, SCM on Windows; complex permissions; Tracked in [#193](https://github.com/mgradwohl/tasksmack/issues/193) |
| **Handles Column** | Task Manager | Open handles (Windows) / file descriptors (Linux); Tracked in [#194](https://github.com/mgradwohl/tasksmack/issues/194) |
| **GDI Objects Column** | Task Manager | Windows-specific (GDI handle count); Tracked in [#195](https://github.com/mgradwohl/tasksmack/issues/195) |
| **Peak Working Set Column** | Task Manager | Historical peak memory usage; Tracked in [#196](https://github.com/mgradwohl/tasksmack/issues/196) |
| **Page Faults Column** | Task Manager | Memory page fault counts; Tracked in [#197](https://github.com/mgradwohl/tasksmack/issues/197) |
| **Base Priority Column** | Task Manager | Windows thread priority class; Tracked in [#198](https://github.com/mgradwohl/tasksmack/issues/198) |
| **CPU Affinity Column** | Task Manager | Which CPU cores the process can use; Tracked in [#199](https://github.com/mgradwohl/tasksmack/issues/199) |
| **Mutation testing** | Development | **Blocked.** Mull requires LLVM 14-17. Clang 22 needs newer mull support; Tracked in [#140](https://github.com/mgradwohl/tasksmack/issues/140) |
| **C++20/23 module support** | Development | **Blocked.** `import std;` requires custom-built libc++; clangd support incomplete. Revisit late 2026; Tracked in [#146](https://github.com/mgradwohl/tasksmack/issues/146) |
| **`std::generator` examples** | Development | **Partial support.** libc++ `std::generator` is experimental; may need `-fexperimental-library`; Tracked in [#145](https://github.com/mgradwohl/tasksmack/issues/145) |

### Completed Features

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
| **Column Visibility Toggles** | btop++ | Right-click table header to show/hide columns; persisted to config |
| **Version header generation** | Development | Auto-generating `version.h` header from CMake |
| **`std::print` adoption** | Development | Type-safe, format-string-based output |


