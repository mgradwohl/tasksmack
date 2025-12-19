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

- Platform probes are stateless readers of OS counters.
- Samplers (domain/app) poll probes on background threads and publish raw counters to models.
- Domain code transforms counters into snapshots and maintains history.
- UI (panels) consumes snapshots, renders views through ImGui/ImPlot, and never calls platform APIs directly.
- OpenGL usage is confined to Core/UI (GLFW + ImGui backends).

## Lessons from the Reference Architecture

### Core Library vs. Application Executable

- In this repo today, TaskSmack is built as a single application target with layered modules under `src/`.
- The long-term goal is still to keep the platform/domain math reusable and testable, even if it remains in one binary.
- **Takeaway:** keep OS probes, domain math, and UI responsibilities separated so splitting into libraries later is low-risk.

### Layer-Based Composition

- Layers expose `onAttach`, `onDetach`, `onUpdate(delta)`, `onRender`, `onPostRender`.
- Layers update/render sequentially; input is handled via ImGui input state.
- **Takeaway:** model TaskSmack panes (processes, metrics, details) as panels, hosted/managed by a top-level shell layer on the layer stack.

### Window Owns OS Events, Application Routes Them

- GLFW receives native callbacks.
- Core registers callbacks to update window state (e.g., framebuffer size) and relies on `glfwPollEvents()`.
- Application polls GLFW each frame; layers/panels react via ImGui input state rather than a custom event bus.
- **Takeaway:** keep GLFW-specific window/input plumbing inside Core; keep the rest of the system platform-agnostic.

## Repository Layout

```
TaskSmack/
  CMakeLists.txt
  CMakePresets.json
  assets/
  src/
    App/
    Core/
    Domain/
    Platform/
    UI/
    main.cpp
  tests/
  tools/
```

## Module Responsibilities

### src/Core

- Owns the application loop and layer stack.
- Creates and manages the GLFW window (and thereby the OpenGL context).
- Provides time utilities, logging bootstrap, and shutdown coordination.
- Contains zero platform metrics code and only minimal OpenGL usage for context creation.

### src/UI

- Configures Dear ImGui and ImPlot (contexts, styling, ini persistence).
- Hooks the ImGui GLFW and OpenGL3 backends.
- Hosts shared widgets, tables, and chart components.
- Consumes immutable domain snapshots plus renderer-provided frame info.
- Contains no direct OS calls outside the ImGui backends.

### src/App

- Owns application panels and UI composition.
- Wires Domain models/samplers into UI rendering.
- Implements panel lifecycle (`onAttach`, `onDetach`, `render`).

### src/Domain

- Defines immutable snapshot types representing system state.
- Implements history buffers with decimation (`History<T>` ring buffers).
- Owns `ProcessModel`, `SystemModel`, and rate calculations derived from counter deltas.
- Enforces a cross-platform metrics contract (CPU% semantics, PID reuse handling, delta-based rates).
- Deterministic and unit-testable; no GLFW, OpenGL, or OS calls.

### src/Platform

- Declares probe interfaces (`IProcessProbe`, `ISystemProbe`, `INetworkProbe`, `IDiskProbe`, ...).
- Provides platform implementations under `windows/` and `linux/`.
- Provides stateless reads of raw counters from OS APIs.
- Produces raw measurements only; domain transforms them into UI-ready data.

## Dependency Direction

```
App/UI
  ↓
Core
  ↓
Domain
  ↑
Platform
  ↑
OS APIs
```

Rules:
- Domain depends on nothing else.
- UI never calls platform APIs.
- Platform never depends on UI or renderer.
- OpenGL usage is confined to Core/UI only.

## UI Layer Model

- **ShellLayer:** docking root, main menu bar, global settings (refresh cadence, theme, column visibility), shared selection state.
- **ProcessesPanel:** process list with sorting and details selection.
- **ProcessDetailsPanel:** detailed view for the currently selected process.
- **SystemMetricsPanel:** plots and timelines backed by domain history.

## Sampling and Snapshot Pipeline

1. **Sampler threads (domain/app)** poll probes on fixed intervals to capture process tables and counters.
2. **Domain models** compute deltas and derived rates (CPU%, IO/s, etc), producing snapshots keyed by PID + start time and updating histories.
3. **UI thread** reads the latest snapshots thread-safely (e.g., copy under a lock) and renders without blocking on sampling.

## Process Scalability Guidance

- Maintain a stable cache keyed by PID + start time to cope with PID reuse.
- Build hierarchical process trees incrementally or during sampling, not every frame on the UI thread.
- Perform sorting and filtering off the UI thread and publish ready-to-render view models.

## OpenGL + GLFW Integration Details

- GLFW handles window creation, input, DPI, framebuffer scaling, and multi-viewport support.
- OpenGL core profile (3.3+) is recommended; only the renderer and ImGui backend issue GL calls.
- ImGui integrations: `imgui_impl_glfw` for events and `imgui_impl_opengl3` for rendering.
- GLFW callbacks update window state; input is handled via ImGui input state.

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

## Platform-Specific Capabilities and Limitations

TaskSmack uses a capability-based system to handle platform differences gracefully. The UI automatically adapts based on what each OS provides.

### Windows Capabilities

**Supported:**
- Per-core CPU metrics (via `NtQuerySystemInformation`)
- Process enumeration with full details (PID, name, command line, user, memory, I/O counters)
- Process CPU times (user + system)
- Memory metrics (total, available, free, swap)
- System uptime and boot timestamp
- CPU base frequency (from registry)
- Process priority (mapped to nice-like values: -20 to +19)
- Process termination and killing
- Thread counts per process

**Not Supported (OS limitations):**
- **I/O wait time** (`iowait`): Windows doesn't expose this metric; it's a Linux-specific concept
- **Steal time** (`steal`): Only meaningful in virtualized environments with specific hypervisors
- **Load average**: Windows has no equivalent to Unix load average (1/5/15 minute averages)
- **Process stop/continue**: Windows doesn't have SIGSTOP/SIGCONT equivalents
- **Shared memory per process**: Windows doesn't expose shared memory pages the same way Linux `/proc/[pid]/statm` does

### Linux Capabilities

**Supported:**
- All Windows capabilities above, plus:
- **I/O wait time** (`iowait`): Time spent waiting for I/O to complete
- **Steal time** (`steal`): Time stolen by hypervisor for other VMs (virtualization-aware)
- **Load average**: 1, 5, and 15 minute load averages from `/proc/loadavg`
- **Process stop/continue**: Full signal support including SIGSTOP, SIGCONT
- **Shared memory per process**: From `/proc/[pid]/statm`
- **Process I/O counters**: Requires root access to `/proc/[pid]/io`

**Limitations:**
- Process I/O counters (`readBytes`, `writeBytes`) require root/elevated privileges to access `/proc/[pid]/io`

### Capability Reporting

Each probe reports its capabilities via the `capabilities()` method:

```cpp
// Example: Check what the current platform supports
auto systemProbe = Platform::makeSystemProbe();
auto caps = systemProbe->capabilities();

if (caps.hasLoadAvg) {
    // Display load average (Linux only)
}

if (caps.hasIoWait) {
    // Display I/O wait percentage (Linux only)
}
```

The UI uses these capabilities to:
- Hide unavailable columns/metrics automatically
- Show platform-appropriate tooltips and help text
- Avoid attempting unsupported operations (e.g., process stop on Windows)

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

## Archived: Process Enumeration Implementation Plan

This section was originally captured in a now-removed `process.md` file as a detailed implementation plan.

**Note:** this appendix is historical and is not guaranteed to match the current code. Treat the actual source in `src/Platform/`, `src/Domain/`, and `src/App/` as canonical.

### Design Philosophy

This plan proposed using **probe interfaces** that collect **raw counters**, with the
**domain layer** computing deltas and rates. This separates OS-specific code from math/logic, enabling:

- Unit-testable domain calculations
- Clean platform boundary (no `#ifdef` soup)
- Consistent semantics across OSes
- Flexible smoothing/windowing options

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         UI Layer                                │
│  (reads immutable snapshots, never calls probes directly)       │
└─────────────────────────────────────────────────────────────────┘
                ▲
                │ reads
┌─────────────────────────────────────────────────────────────────┐
│                       Domain Layer                              │
│  - Snapshot structs (immutable, POD-like)                       │
│  - Calculators (compute deltas, rates, percentages)             │
│  - History buffers (ring buffers with decimation)               │
└─────────────────────────────────────────────────────────────────┘
                ▲
                │ raw counters
┌─────────────────────────────────────────────────────────────────┐
│                      Platform Layer                             │
│  - Probe interfaces (ICpuProbe, IProcessProbe, etc.)            │
│  - Platform implementations (LinuxCpuProbe, WindowsCpuProbe)    │
│  - Factory function (make_platform_probes())                    │
└─────────────────────────────────────────────────────────────────┘
                ▲
                │ OS APIs
┌─────────────────────────────────────────────────────────────────┐
│  Linux: /proc/*, sysfs      │  Windows: NtQuery*, ToolHelp32    │
└─────────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

#### 1. Probes Return Raw Counters, Not Final Values

**Why:** CPU% and rates require deltas over time. Probes should be stateless readers; domain owns the math.

```cpp
// WRONG: Probe computes final value (hides state, untestable math)
virtual double getCpuPercent() = 0;

// RIGHT: Probe returns raw counters (domain computes from deltas)
virtual CpuCounters readCounters() = 0;
```

#### 2. Many Small Probes (Not One God-Object)

Each metric group gets its own probe interface:

- `IProcessProbe` - process enumeration
- `ICpuProbe` - CPU counters (total + per-core)
- `IMemoryProbe` - memory counters
- `INetProbe` - network interface stats (future)
- `IDiskProbe` - disk I/O stats (future)

**Pros:** Clear responsibilities, easy to stub in tests, incremental implementation.

#### 3. Capability Reporting

OSes expose different data. Probes report what they support:

```cpp
struct ProcessCapabilities {
  bool hasIoCounters = false;
  bool hasThreadCount = false;
  bool hasStartTime = true;
};

virtual ProcessCapabilities capabilities() const = 0;
```

UI degrades gracefully (hides unavailable columns).

#### 4. Factory Pattern for Platform Selection

Single `#ifdef` location:

```cpp
// Platform/Factory.h
std::unique_ptr<IProcessProbe> makeProcessProbe();
std::unique_ptr<ICpuProbe> makeCpuProbe();

// Platform/Linux/Factory.cpp (compiled on Linux)
std::unique_ptr<IProcessProbe> makeProcessProbe() {
  return std::make_unique<LinuxProcessProbe>();
}

// Platform/Windows/Factory.cpp (compiled on Windows)
std::unique_ptr<IProcessProbe> makeProcessProbe() {
  return std::make_unique<WindowsProcessProbe>();
}
```

#### 5. Explicit Sampling Model

The proposed sampler runs on a background thread at a configured interval (e.g., 1 second):

```
Sampler Thread                    Domain                      UI Thread
   │                              │                             │
   │──── readCounters() ─────────►│                             │
   │                              │── compute(prev, cur) ──────►│
   │                              │   publish(snapshot)         │
   │                              │                             │
   │        (1 second later)      │                             │
   │──── readCounters() ─────────►│                             │
   │                              │── compute(prev, cur) ──────►│
   │                              │   publish(snapshot)         │
```

### Phase 1: Process Enumeration (This Implementation)

#### File Structure

```
src/
├── Platform/
│   ├── ProcessTypes.h           # Raw counter structs
│   ├── IProcessProbe.h          # Probe interface
│   ├── Factory.h                # Factory declarations
│   ├── Linux/
│   │   ├── LinuxProcessProbe.h
│   │   ├── LinuxProcessProbe.cpp
│   │   └── Factory.cpp
│   └── Windows/
│       ├── WindowsProcessProbe.h
│       ├── WindowsProcessProbe.cpp
│       └── Factory.cpp
├── Domain/
│   ├── ProcessSnapshot.h        # Immutable snapshot struct
│   ├── ProcessModel.h           # Calculator + cache
│   └── ProcessModel.cpp
└── App/
  ├── ShellLayer.h             # (modified)
  └── ShellLayer.cpp           # (modified)
```

#### Platform Layer: Raw Counters

```cpp
// Platform/ProcessTypes.h
namespace Platform {

// Raw counters from OS - no computed values
struct ProcessCounters {
  int32_t pid = 0;
  int32_t parentPid = 0;
  std::string name;
  std::string state;           // Raw state char/string from OS
  uint64_t startTimeTicks = 0; // For PID reuse detection

  // CPU time (cumulative ticks or jiffies)
  uint64_t userTime = 0;
  uint64_t systemTime = 0;

  // Memory (bytes)
  uint64_t rssBytes = 0;
  uint64_t virtualBytes = 0;

  // Optional fields (check capabilities)
  uint64_t readBytes = 0;
  uint64_t writeBytes = 0;
  int32_t threadCount = 0;
};

struct ProcessCapabilities {
  bool hasIoCounters = false;
  bool hasThreadCount = false;
  bool hasUserSystemTime = true;
  bool hasStartTime = true;
};

} // namespace Platform
```

#### Platform Layer: Probe Interface

```cpp
// Platform/IProcessProbe.h
namespace Platform {

class IProcessProbe {
public:
  virtual ~IProcessProbe() = default;

  // Returns raw counters for all processes (stateless read)
  virtual std::vector<ProcessCounters> enumerate() = 0;

  // What this platform supports
  virtual ProcessCapabilities capabilities() const = 0;

  // System-wide values needed for CPU% calculation
  virtual uint64_t totalCpuTime() const = 0;
  virtual long ticksPerSecond() const = 0;
};

} // namespace Platform
```

#### Domain Layer: Computed Snapshot

```cpp
// Domain/ProcessSnapshot.h
namespace Domain {

// Immutable, UI-ready data (computed from counter deltas)
struct ProcessSnapshot {
  int32_t pid = 0;
  int32_t parentPid = 0;
  std::string name;
  std::string displayState;    // "Running", "Sleeping", etc.

  double cpuPercent = 0.0;     // Computed from deltas
  uint64_t memoryBytes = 0;
  uint64_t virtualBytes = 0;

  // Optional (may be 0 if not supported)
  double ioReadBytesPerSec = 0.0;
  double ioWriteBytesPerSec = 0.0;
  int32_t threadCount = 0;

  // For stable identity across samples
  uint64_t uniqueKey = 0;      // hash(pid, startTime)
};

} // namespace Domain
```

#### Domain Layer: Model (Calculator + Cache)

```cpp
// Domain/ProcessModel.h
namespace Domain {

class ProcessModel {
public:
  explicit ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe);

  // Call periodically (e.g., every 1 second)
  void refresh();

  // Get latest computed snapshots (thread-safe read)
  const std::vector<ProcessSnapshot>& snapshots() const;

  // Stats
  size_t processCount() const;

private:
  std::unique_ptr<Platform::IProcessProbe> m_Probe;

  // Previous counters for delta calculation (keyed by uniqueKey)
  std::unordered_map<uint64_t, Platform::ProcessCounters> m_PrevCounters;
  uint64_t m_PrevTotalCpuTime = 0;

  // Latest computed snapshots
  std::vector<ProcessSnapshot> m_Snapshots;

  // Helpers
  ProcessSnapshot computeSnapshot(
    const Platform::ProcessCounters& current,
    const Platform::ProcessCounters* previous,
    uint64_t totalCpuDelta) const;

  static uint64_t makeUniqueKey(int32_t pid, uint64_t startTime);
  static std::string translateState(const std::string& rawState);
};

} // namespace Domain
```

#### Linux Implementation

```cpp
// Platform/Linux/LinuxProcessProbe.cpp

// Reads from:
// - /proc/[pid]/stat     -> pid, name, state, ppid, utime, stime, starttime
// - /proc/[pid]/statm    -> rss, vsize (in pages)
// - /proc/[pid]/io       -> read_bytes, write_bytes (optional, needs permissions)
// - /proc/stat           -> total CPU time (for calculating %)
// - sysconf(_SC_CLK_TCK) -> ticks per second
```

#### CPU% Calculation

```cpp
// In ProcessModel::computeSnapshot()

// CPU% = (processCpuDelta / totalCpuDelta) * 100
// where:
//   processCpuDelta = (current.userTime + current.systemTime)
//                   - (previous.userTime + previous.systemTime)
//   totalCpuDelta   = currentTotalCpu - previousTotalCpu

if (previous != nullptr && totalCpuDelta > 0) {
  uint64_t processDelta = (current.userTime + current.systemTime)
              - (previous->userTime + previous->systemTime);
  snapshot.cpuPercent = (static_cast<double>(processDelta) /
              static_cast<double>(totalCpuDelta)) * 100.0;
}
```

#### UI Integration

```cpp
// In ShellLayer

class ShellLayer : public Core::Layer {
private:
  std::unique_ptr<Domain::ProcessModel> m_ProcessModel;
  float m_RefreshTimer = 0.0F;
  static constexpr float REFRESH_INTERVAL = 1.0F;  // seconds

public:
  void onAttach() override {
    m_ProcessModel = std::make_unique<Domain::ProcessModel>(
      Platform::makeProcessProbe()
    );
    m_ProcessModel->refresh();  // Initial population
  }

  void onUpdate(float deltaTime) override {
    m_RefreshTimer += deltaTime;
    if (m_RefreshTimer >= REFRESH_INTERVAL) {
      m_ProcessModel->refresh();
      m_RefreshTimer = 0.0F;
    }
  }

  void renderProcessesPanel() {
    for (const auto& proc : m_ProcessModel->snapshots()) {
      // Render row with proc.pid, proc.name, proc.cpuPercent, etc.
    }
  }
};
```

## CMake Integration

```cmake
# Conditional platform sources
if(WIN32)
  set(PLATFORM_SOURCES
    src/Platform/Windows/WindowsProcessProbe.cpp
    src/Platform/Windows/Factory.cpp
  )
else()
  set(PLATFORM_SOURCES
    src/Platform/Linux/LinuxProcessProbe.cpp
    src/Platform/Linux/Factory.cpp
  )
endif()

set(TASKSMACK_SOURCES
  ${TASKSMACK_SOURCES}
  src/Domain/ProcessModel.cpp
  ${PLATFORM_SOURCES}
)
```

### Implementation Order

1. **Pick an issue** (or create one) and clarify acceptance criteria.
2. **Create a branch** for the work.
3. **Create Platform types and interface** (`ProcessTypes.h`, `IProcessProbe.h`)
4. **Create Domain snapshot and model** (`ProcessSnapshot.h`, `ProcessModel.h/cpp`)
5. **Implement Linux probe** (`LinuxProcessProbe.cpp`)
6. **Implement Windows probe** (`WindowsProcessProbe.cpp`)
7. **Create factory for both platforms** (`Factory.h`, `Linux/Factory.cpp`, `Windows/Factory.cpp`)
8. **Integrate with ShellLayer** (add ProcessModel, wire up refresh)
9. **Update CMakeLists.txt** (add new sources and tests)
10. **Write automated tests** (domain calculations + platform contract)
11. **Build and test**

### Future Phases

#### Phase 2: System Metrics

- `ICpuProbe` - total/per-core CPU counters
- `IMemoryProbe` - system memory counters
- Domain calculators for CPU% over time, memory trends

#### Phase 3: Background Sampling

- `std::jthread` sampler with `std::stop_token`
- Lock-free snapshot publishing
- Configurable refresh intervals

#### Phase 4: Process Actions

- Kill, suspend, resume processes
- Priority adjustment
- Requires elevated permissions handling

### Testing Strategy

- **Domain tests:** Feed mock `ProcessCounters` to `ProcessModel`, verify CPU% math
- **Platform tests:** Run on actual OS, verify probe reads real data
- **Integration tests:** Full stack with short refresh interval

### Notes

- PID reuse: Handled by `uniqueKey = hash(pid, startTime)`
- Permissions: Some Linux `/proc/[pid]/io` requires same-user or root
- Performance: Enumerate all PIDs at 1 Hz is fine; optimize if needed later
