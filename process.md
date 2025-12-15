# TaskSmack Process Enumeration Implementation Plan

## Design Philosophy

Based on architectural review, we're using **probe interfaces** that collect **raw counters**, with **domain layer** computing deltas and rates. This separates OS-specific code from math/logic, enabling:

- Unit-testable domain calculations
- Clean platform boundary (no `#ifdef` soup)
- Consistent semantics across OSes
- Flexible smoothing/windowing options

## Architecture

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

## Key Design Decisions

### 1. Probes Return Raw Counters, Not Final Values

**Why:** CPU% and rates require deltas over time. Probes should be stateless readers; domain owns the math.

```cpp
// WRONG: Probe computes final value (hides state, untestable math)
virtual double getCpuPercent() = 0;

// RIGHT: Probe returns raw counters (domain computes from deltas)
virtual CpuCounters readCounters() = 0;
```

### 2. Many Small Probes (Not One God-Object)

Each metric group gets its own probe interface:
- `IProcessProbe` - process enumeration
- `ICpuProbe` - CPU counters (total + per-core)
- `IMemoryProbe` - memory counters
- `INetProbe` - network interface stats (future)
- `IDiskProbe` - disk I/O stats (future)

**Pros:** Clear responsibilities, easy to stub in tests, incremental implementation.

### 3. Capability Reporting

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

### 4. Factory Pattern for Platform Selection

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

### 5. Explicit Sampling Model

Sampler runs on background thread at configured interval (e.g., 1 second):

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

## Phase 1: Process Enumeration (This Implementation)

### File Structure

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
│       ├── WindowsProcessProbe.cpp  # (stub initially)
│       └── Factory.cpp
├── Domain/
│   ├── ProcessSnapshot.h        # Immutable snapshot struct
│   ├── ProcessModel.h           # Calculator + cache
│   └── ProcessModel.cpp
└── App/
    ├── ShellLayer.h             # (modified)
    └── ShellLayer.cpp           # (modified)
```

### Platform Layer: Raw Counters

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

### Platform Layer: Probe Interface

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

### Domain Layer: Computed Snapshot

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

### Domain Layer: Model (Calculator + Cache)

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

### Linux Implementation

```cpp
// Platform/Linux/LinuxProcessProbe.cpp

// Reads from:
// - /proc/[pid]/stat     -> pid, name, state, ppid, utime, stime, starttime
// - /proc/[pid]/statm    -> rss, vsize (in pages)
// - /proc/[pid]/io       -> read_bytes, write_bytes (optional, needs permissions)
// - /proc/stat           -> total CPU time (for calculating %)
// - sysconf(_SC_CLK_TCK) -> ticks per second
```

### CPU% Calculation

```cpp
// In ProcessModel::computeSnapshot()

// CPU% = (processCpuDelta / totalCpuDelta) * 100 * numCores
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

### UI Integration

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

## Implementation Order

1. **Create Platform types and interface** (`ProcessTypes.h`, `IProcessProbe.h`)
2. **Create Domain snapshot and model** (`ProcessSnapshot.h`, `ProcessModel.h/cpp`)
3. **Implement Linux probe** (`LinuxProcessProbe.cpp`)
4. **Create factory** (`Factory.h`, `Linux/Factory.cpp`)
5. **Integrate with ShellLayer** (add ProcessModel, wire up refresh)
6. **Update CMakeLists.txt** (add new sources)
7. **Build and test**
8. **Add Windows stub** (returns empty vector, for cross-platform builds)

## Future Phases

### Phase 2: System Metrics
- `ICpuProbe` - total/per-core CPU counters
- `IMemoryProbe` - system memory counters
- Domain calculators for CPU% over time, memory trends

### Phase 3: Background Sampling
- `std::jthread` sampler with `std::stop_token`
- Lock-free snapshot publishing
- Configurable refresh intervals

### Phase 4: Process Actions
- Kill, suspend, resume processes
- Priority adjustment
- Requires elevated permissions handling

## Testing Strategy

- **Domain tests:** Feed mock `ProcessCounters` to `ProcessModel`, verify CPU% math
- **Platform tests:** Run on actual OS, verify probe reads real data
- **Integration tests:** Full stack with short refresh interval

## Notes

- PID reuse: Handled by `uniqueKey = hash(pid, startTime)`
- Permissions: Some Linux `/proc/[pid]/io` requires same-user or root
- Performance: Enumerate all PIDs at 1 Hz is fine; optimize if needed later
