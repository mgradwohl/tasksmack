# TaskSmack GPU Support Plan and Architecture

**Status:** Planning Phase (No Implementation Yet)  
**Created:** 2025-12-31  
**Related Issues:** #178, #189, #176, #300

## Executive Summary

This document outlines a comprehensive plan for adding GPU monitoring capabilities to TaskSmack. The design follows TaskSmack's established layered architecture (Platform → Domain → UI) and provides cross-platform support for Windows and Linux with graceful degradation for unsupported hardware/configurations.

### Key Features

1. **System-level GPU metrics** with history graphs in System Overview panel
2. **Per-process GPU usage** in process table and Process Details panel
3. **Multi-GPU support** for systems with multiple discrete or integrated GPUs
   - Automatic detection of integrated vs discrete GPUs
   - Smart default display (GPU with highest utilization)
   - User-selectable GPU for detailed metrics
   - Aggregated GPU usage when processes span multiple GPUs
4. **Comprehensive vendor support** with graceful degradation:
   - **NVIDIA** (NVML): Full feature set on Windows and Linux
   - **AMD** (ROCm/ADL): Full support on Linux, DXGI-based on Windows
   - **Intel** (DRM/sysfs): Basic metrics on Linux, DXGI-based on Windows
   - **Generic fallback**: DXGI (Windows), DRM (Linux) when vendor libraries unavailable
5. **GPU engine tracking**: Which GPU engines are active (3D, Video Encode/Decode, Compute, Copy)
6. **Temperature monitoring**: GPU die temperature and hotspot temperatures
7. **Memory tracking**: VRAM usage (allocated, free, total) per GPU and per process
8. **Theme-aware UI**: All GPU colors sourced from theme files; icons from Font Awesome
9. **Guided installation**: UI prompts for installing vendor SDKs to unlock additional features

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [GPU Metrics Specification](#gpu-metrics-specification)
3. [Platform Layer Design](#platform-layer-design)
4. [Domain Layer Design](#domain-layer-design)
5. [UI Layer Design](#ui-layer-design)
6. [Windows Implementation Plan](#windows-implementation-plan)
7. [Linux Implementation Plan](#linux-implementation-plan)
8. [Library Dependencies and CMake Integration](#library-dependencies-and-cmake-integration)
9. [Testing Strategy](#testing-strategy)
10. [Implementation Phases](#implementation-phases)
11. [Security and Performance Considerations](#security-and-performance-considerations)
12. [Future Enhancements](#future-enhancements)

---

## Architecture Overview

### Layered Design

Following TaskSmack's established patterns:

```
┌─────────────────────────────────────────────────────────────────┐
│                         UI Layer                                │
│  - System Overview: GPU history graphs + "now bars"            │
│  - Process Table: GPU%, GPU Engine, GPU Memory columns         │
│  - Process Details: Per-process GPU metrics + history          │
└─────────────────────────────────────────────────────────────────┘
                 ▲ reads immutable snapshots
┌─────────────────────────────────────────────────────────────────┐
│                       Domain Layer                              │
│  - GPUSnapshot: Immutable per-GPU state                        │
│  - GPUModel: Computes utilization %, rates, history           │
│  - ProcessSnapshot: Extended with GPU fields                   │
│  - ProcessModel: Integrates per-process GPU metrics            │
└─────────────────────────────────────────────────────────────────┘
                 ▲ transforms raw counters
┌─────────────────────────────────────────────────────────────────┐
│                      Platform Layer                             │
│  - IGPUProbe: Abstract interface for GPU enumeration/metrics   │
│  - GPUTypes.h: Raw counter structs (vendor-agnostic)           │
│  - Implementations:                                             │
│    • NVMLGPUProbe (NVIDIA, cross-platform)                     │
│    • ROCmGPUProbe (AMD, Linux)                                 │
│    • DRMGPUProbe (Intel/AMD, Linux fallback)                   │
│    • DXGIGPUProbe (Windows, all vendors)                       │
│    • D3DKMTGPUProbe (Windows, per-process attribution)         │
└─────────────────────────────────────────────────────────────────┘
                 ▲ OS/vendor APIs
┌─────────────────────────────────────────────────────────────────┐
│  Linux: DRM, NVML, ROCm, sysfs   │  Windows: DXGI, D3DKMT, NVML│
└─────────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **Vendor Abstraction**: Platform layer provides unified interface; vendor-specific details hidden
2. **Graceful Degradation**: UI adapts based on `GPUCapabilities`; missing features are hidden
3. **Raw Counters Only**: Probes return cumulative counters; Domain computes rates and percentages
4. **Multi-GPU Native**: Architecture supports 0-N GPUs from the start
5. **Optional Dependencies**: GPU libraries (NVML, ROCm) are optional; system works without them
6. **Testability**: Domain logic is unit-testable with mock probes
7. **All Vendors Supported**: NVIDIA, AMD, Intel all supported with vendor-specific limitations clearly documented
8. **User Guidance**: UI provides actionable guidance for installing vendor SDKs to unlock features
9. **Smart Multi-GPU**: Default to showing GPU with highest utilization; allow user to view all or select specific GPU
10. **GPU Type Awareness**: UI differentiates integrated vs discrete GPUs with appropriate labeling and icons
11. **Theme Integration**: All colors from theme files; GPU gets dedicated theme color section

---

## GPU Metrics Specification

### System-Level Metrics

Each GPU exposes:

| Metric | Type | Description | Source |
|--------|------|-------------|--------|
| **Utilization** | `double` | GPU core utilization 0-100% | Vendor API |
| **Memory Utilization** | `double` | VRAM usage 0-100% | Vendor API |
| **Memory Used** | `uint64_t` | VRAM allocated in bytes | Vendor API |
| **Memory Total** | `uint64_t` | Total VRAM in bytes | Vendor API |
| **Temperature** | `int32_t` | GPU die temperature in °C | Vendor API |
| **Hotspot Temperature** | `int32_t` | Hotspot temp in °C (if available) | Vendor API |
| **Power Draw** | `double` | Current power consumption in watts | Vendor API |
| **Power Limit** | `double` | Power limit in watts | Vendor API |
| **Clock Speed** | `uint32_t` | Current GPU clock in MHz | Vendor API |
| **Memory Clock** | `uint32_t` | Current memory clock in MHz | Vendor API |
| **Fan Speed** | `uint32_t` | Fan speed in RPM (if available) | Vendor API |
| **PCIe Bandwidth** | `uint64_t` | Current PCIe throughput in bytes/sec | Vendor API |

### Per-Process Metrics

Associated with each process:

| Metric | Type | Description | Source |
|--------|------|-------------|--------|
| **GPU Utilization** | `double` | Process GPU usage 0-100% | D3DKMT (Win), NVML (Linux) |
| **GPU Memory** | `uint64_t` | VRAM allocated by process in bytes | D3DKMT (Win), NVML (Linux) |
| **GPU Engine** | `std::string` | Active engine(s): "3D", "Video", "Compute", "Copy" | D3DKMT (Win), vendor-specific |
| **Encoder Utilization** | `double` | Video encoder usage 0-100% | Vendor API |
| **Decoder Utilization** | `double` | Video decoder usage 0-100% | Vendor API |

### GPU Engine Types

Processes can use multiple engines simultaneously:

- **3D/Graphics**: Rendering, graphics pipeline
- **Compute**: CUDA, OpenCL, DirectCompute, Vulkan Compute
- **Video Encode**: Hardware video encoding (NVENC, VCE, QuickSync)
- **Video Decode**: Hardware video decoding (NVDEC, VCN, QuickSync)
- **Copy/DMA**: Memory transfers, display output

### Vendor Capability Matrix

This matrix shows which features are available per vendor and OS. The UI uses this to:
- Hide unavailable features automatically
- Display vendor-specific tooltips
- Provide installation guidance for missing SDKs

| Feature | NVIDIA (NVML) | AMD (ROCm/ADL) | Intel | Generic (DXGI/DRM) |
|---------|---------------|----------------|-------|-------------------|
| **System GPU Utilization** | ✅ Win/Linux | ✅ Linux / ⚠️ Win (basic) | ✅ Linux / ⚠️ Win (basic) | ✅ Win/Linux (basic) |
| **GPU Memory Used/Total** | ✅ Win/Linux | ✅ Linux / ✅ Win | ✅ Linux / ✅ Win | ✅ Win/Linux |
| **Temperature** | ✅ Win/Linux | ✅ Linux / ⚠️ Win (limited) | ✅ Linux / ❌ Win | ⚠️ sysfs/hwmon only |
| **Power Draw** | ✅ Win/Linux | ✅ Linux / ❌ Win | ⚠️ Linux (limited) / ❌ Win | ❌ |
| **Clock Speeds** | ✅ Win/Linux | ✅ Linux / ⚠️ Win (limited) | ✅ Linux / ❌ Win | ⚠️ sysfs only |
| **Fan Speed** | ✅ Win/Linux | ✅ Linux / ❌ Win | ❌ | ❌ |
| **PCIe Bandwidth** | ✅ Win/Linux | ⚠️ Linux (limited) / ❌ Win | ⚠️ sysfs only / ❌ Win | ❌ |
| **Per-Process GPU %** | ✅ Win/Linux | ❌ Linux / ✅ Win (D3DKMT) | ❌ Linux / ✅ Win (D3DKMT) | ⚠️ D3DKMT (Win) / ❌ Linux |
| **Per-Process GPU Memory** | ✅ Win/Linux | ❌ Linux / ✅ Win (D3DKMT) | ❌ Linux / ✅ Win (D3DKMT) | ✅ Win (D3DKMT) / ❌ Linux |
| **GPU Engine Attribution** | ⚠️ Via compute mode / ✅ Win (D3DKMT) | ❌ Linux / ✅ Win (D3DKMT) | ❌ Linux / ✅ Win (D3DKMT) | ✅ Win (D3DKMT) / ❌ Linux |
| **Encoder/Decoder Util** | ✅ Win/Linux | ⚠️ Linux (limited) / ❌ Win | ❌ | ❌ |
| **Multi-GPU Support** | ✅ Win/Linux | ✅ Win/Linux | ✅ Win/Linux | ✅ Win/Linux |

**Legend:**
- ✅ = Fully supported with accurate metrics
- ⚠️ = Partial support or limited accuracy (details in notes)
- ❌ = Not available on this platform/vendor combination

**Important Limitations:**

1. **AMD on Windows**: Basic metrics via DXGI only (no ROCm); per-process via D3DKMT; no power/temperature without vendor tools
2. **AMD on Linux**: No per-process GPU utilization (ROCm limitation); use DXGI-equivalent fallback for basic system metrics
3. **Intel on Windows**: Basic metrics via DXGI; per-process via D3DKMT; no temperature or power metrics
4. **Intel on Linux**: Basic metrics via DRM/sysfs; no per-process metrics; limited temperature (hwmon)
5. **Per-Process on Linux**: Only NVIDIA via NVML provides per-process GPU utilization; AMD/Intel show memory only
6. **GPU Engine Attribution**: Most detailed on Windows via D3DKMT; NVIDIA NVML provides compute mode tracking

**Recommended Installation Guidance (shown in UI when features unavailable):**

| Vendor | Platform | Missing SDK | User Guidance |
|--------|----------|-------------|---------------|
| NVIDIA | Windows/Linux | NVIDIA Driver | "Install NVIDIA drivers from nvidia.com to enable GPU monitoring" |
| AMD | Linux | ROCm | "Install ROCm from amd.com/rocm for enhanced AMD GPU metrics" |
| Intel | Linux | intel-gpu-tools | "Install intel-gpu-tools package for enhanced Intel GPU metrics" |
| Any | Linux | libdrm | "Install libdrm-dev package for basic GPU enumeration" |

---

## Platform Layer Design

### File Structure

```
src/Platform/
├── GPUTypes.h               # Raw counter structs (vendor-agnostic)
├── IGPUProbe.h             # Abstract probe interface
├── Factory.h               # Extended with makeGPUProbe()
├── Linux/
│   ├── LinuxGPUProbe.h     # Composite Linux GPU probe
│   ├── LinuxGPUProbe.cpp
│   ├── NVMLGPUProbe.h      # NVIDIA-specific (Linux)
│   ├── NVMLGPUProbe.cpp
│   ├── ROCmGPUProbe.h      # AMD-specific (Linux)
│   ├── ROCmGPUProbe.cpp
│   ├── DRMGPUProbe.h       # Fallback DRM probe
│   ├── DRMGPUProbe.cpp
│   └── Factory.cpp         # Updated
└── Windows/
    ├── WindowsGPUProbe.h   # Composite Windows GPU probe
    ├── WindowsGPUProbe.cpp
    ├── DXGIGPUProbe.h      # DXGI for system metrics
    ├── DXGIGPUProbe.cpp
    ├── D3DKMTGPUProbe.h    # D3DKMT for per-process
    ├── D3DKMTGPUProbe.cpp
    ├── NVMLGPUProbe.h      # NVIDIA-specific (Windows)
    ├── NVMLGPUProbe.cpp
    └── Factory.cpp         # Updated
```

### GPUTypes.h

```cpp
namespace Platform {

// Identifies a physical GPU
struct GPUInfo {
    std::string id;              // Unique identifier (PCI address, UUID)
    std::string name;            // Human-readable name
    std::string vendor;          // "NVIDIA", "AMD", "Intel", "Unknown"
    std::string driverVersion;
    bool isIntegrated = false;   // Integrated vs discrete
    uint32_t deviceIndex = 0;    // Vendor-specific index
};

// Raw GPU counters (cumulative where applicable)
struct GPUCounters {
    std::string gpuId;           // Associates with GPUInfo

    // Utilization (instantaneous snapshot, not cumulative)
    double utilizationPercent = 0.0;  // 0-100
    double memoryUtilPercent = 0.0;   // 0-100

    // Memory (bytes)
    uint64_t memoryUsedBytes = 0;
    uint64_t memoryTotalBytes = 0;

    // Temperature (°C)
    int32_t temperatureC = 0;
    int32_t hotspotTempC = 0;  // -1 if not available

    // Power (watts)
    double powerDrawWatts = 0.0;
    double powerLimitWatts = 0.0;

    // Clock speeds (MHz)
    uint32_t gpuClockMHz = 0;
    uint32_t memoryClockMHz = 0;

    // Fan speed (RPM, 0 if not available)
    uint32_t fanSpeedRPM = 0;

    // PCIe throughput (cumulative bytes)
    uint64_t pcieTxBytes = 0;
    uint64_t pcieRxBytes = 0;

    // Engine utilization (0-100, instantaneous)
    double computeUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;
};

// Per-process GPU usage
struct ProcessGPUCounters {
    int32_t pid = 0;
    std::string gpuId;           // Which GPU

    // Memory allocated by process (bytes)
    uint64_t gpuMemoryBytes = 0;

    // Utilization attributed to this process (0-100, instantaneous)
    double gpuUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;

    // Active engines (bitmask or string set)
    // Engines: 3D, Compute, Video Encode, Video Decode, Copy
    std::vector<std::string> activeEngines;
};

// Capability reporting
struct GPUCapabilities {
    bool hasTemperature = false;
    bool hasHotspotTemp = false;
    bool hasPowerMetrics = false;
    bool hasClockSpeeds = false;
    bool hasFanSpeed = false;
    bool hasPCIeMetrics = false;
    bool hasEngineUtilization = false;
    bool hasPerProcessMetrics = false;  // Per-process GPU usage
    bool hasEncoderDecoder = false;
    bool supportsMultiGPU = false;
};

} // namespace Platform
```

### IGPUProbe.h

```cpp
namespace Platform {

class IGPUProbe {
public:
    virtual ~IGPUProbe() = default;

    // Enumerate available GPUs (called once at startup or on refresh)
    [[nodiscard]] virtual std::vector<GPUInfo> enumerateGPUs() = 0;

    // Read system-level GPU metrics (called every sample interval)
    [[nodiscard]] virtual std::vector<GPUCounters> readGPUCounters() = 0;

    // Read per-process GPU metrics (called every sample interval)
    // Returns empty vector if not supported
    [[nodiscard]] virtual std::vector<ProcessGPUCounters> readProcessGPUCounters() = 0;

    // Capability reporting
    [[nodiscard]] virtual GPUCapabilities capabilities() const = 0;
};

} // namespace Platform
```

### Composite Probe Pattern

On both Windows and Linux, the platform-specific probe will be a **composite** that delegates to vendor-specific sub-probes:

**Linux Example:**
```cpp
class LinuxGPUProbe : public IGPUProbe {
private:
    std::vector<std::unique_ptr<IGPUProbe>> m_VendorProbes;
    // Try NVML, ROCm, DRM in order; keep those that succeed initialization
};
```

**Windows Example:**
```cpp
class WindowsGPUProbe : public IGPUProbe {
private:
    std::unique_ptr<DXGIGPUProbe> m_DXGI;        // System-level metrics
    std::unique_ptr<D3DKMTGPUProbe> m_D3DKMT;    // Per-process attribution
    std::unique_ptr<NVMLGPUProbe> m_NVML;        // NVIDIA-specific
};
```

---

## Domain Layer Design

### File Structure

```
src/Domain/
├── GPUSnapshot.h            # Immutable per-GPU state
├── GPUModel.h              # GPU model + history
├── GPUModel.cpp
├── ProcessSnapshot.h       # Extended with GPU fields
└── ProcessModel.cpp        # Updated to integrate GPU data
```

### GPUSnapshot.h

```cpp
namespace Domain {

// Immutable snapshot of a single GPU at a point in time
struct GPUSnapshot {
    // Identity
    std::string gpuId;
    std::string name;
    std::string vendor;
    bool isIntegrated = false;

    // Utilization (0-100)
    double utilizationPercent = 0.0;
    double memoryUtilPercent = 0.0;

    // Memory
    uint64_t memoryUsedBytes = 0;
    uint64_t memoryTotalBytes = 0;
    double memoryUsedPercent = 0.0;  // Computed by Domain

    // Temperature
    int32_t temperatureC = 0;
    int32_t hotspotTempC = -1;

    // Power
    double powerDrawWatts = 0.0;
    double powerLimitWatts = 0.0;
    double powerUtilPercent = 0.0;  // Computed by Domain

    // Clock speeds
    uint32_t gpuClockMHz = 0;
    uint32_t memoryClockMHz = 0;

    // Fan
    uint32_t fanSpeedRPM = 0;

    // PCIe bandwidth (rates computed from deltas)
    double pcieTxBytesPerSec = 0.0;
    double pcieRxBytesPerSec = 0.0;

    // Engine utilization
    double computeUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;
};

} // namespace Domain
```

### GPUModel Class

```cpp
namespace Domain {

class GPUModel {
public:
    explicit GPUModel(std::unique_ptr<Platform::IGPUProbe> probe);

    // Refresh metrics (called by sampler thread)
    void refresh();

    // Get current snapshots (thread-safe)
    [[nodiscard]] std::vector<GPUSnapshot> snapshots() const;

    // Get history for specific GPU
    [[nodiscard]] const History<GPUSnapshot>& history(const std::string& gpuId) const;

    // GPU info (static, rarely changes)
    [[nodiscard]] std::vector<Platform::GPUInfo> gpuInfo() const;

    // Capabilities
    [[nodiscard]] Platform::GPUCapabilities capabilities() const;

private:
    std::unique_ptr<Platform::IGPUProbe> m_Probe;
    std::vector<Platform::GPUInfo> m_GPUInfo;
    
    // Current snapshots per GPU
    std::unordered_map<std::string, GPUSnapshot> m_Snapshots;

    // History buffers per GPU
    std::unordered_map<std::string, History<GPUSnapshot>> m_Histories;

    // Previous counters for rate calculation
    std::unordered_map<std::string, Platform::GPUCounters> m_PrevCounters;
    std::chrono::steady_clock::time_point m_PrevSampleTime;

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helper: compute snapshot from current/previous counters
    GPUSnapshot computeSnapshot(
        const Platform::GPUCounters& current,
        const Platform::GPUCounters* previous,
        double timeDeltaSeconds) const;
};

} // namespace Domain
```

### ProcessSnapshot Extensions

Extend `Domain::ProcessSnapshot` with GPU fields:

```cpp
// In src/Domain/ProcessSnapshot.h (add these fields)

struct ProcessSnapshot {
    // ... existing fields ...

    // GPU usage (per-process, aggregated across all GPUs)
    double gpuUtilPercent = 0.0;        // Total GPU % across all GPUs process uses
    uint64_t gpuMemoryBytes = 0;        // Total VRAM allocated across all GPUs
    std::vector<std::string> gpuEngines;  // Union of active engines: ["3D", "Compute"]
    double gpuEncoderUtil = 0.0;        // Aggregate encoder utilization
    double gpuDecoderUtil = 0.0;        // Aggregate decoder utilization
    std::string gpuDevices;             // Comma-separated GPU IDs: "0" or "0,1"
    
    // Per-GPU breakdown (for tooltip/details view)
    struct PerGPUUsage {
        std::string gpuId;              // GPU identifier
        std::string gpuName;            // e.g., "NVIDIA RTX 4090"
        bool isIntegrated;              // Integrated vs discrete
        double utilPercent;             // GPU % on this specific GPU
        uint64_t memoryBytes;           // VRAM allocated on this GPU
        std::vector<std::string> engines; // Active engines on this GPU
    };
    std::vector<PerGPUUsage> perGpuUsage; // Breakdown for multi-GPU processes
};
```

**Multi-GPU Aggregation Logic:**

When a process uses multiple GPUs, `ProcessModel` aggregates metrics as follows:

```cpp
// Example: Process uses GPU 0 (30% util, 2GB) and GPU 1 (45% util, 512MB)
snapshot.gpuUtilPercent = 30.0 + 45.0;  // 75% total
snapshot.gpuMemoryBytes = 2GB + 512MB;   // 2.5GB total
snapshot.gpuEngines = {"3D", "Compute"}; // Union from both GPUs
snapshot.gpuDevices = "0,1";             // Comma-separated list

// Per-GPU breakdown for detailed view
snapshot.perGpuUsage = {
    {gpuId: "0", gpuName: "NVIDIA RTX 4090", isIntegrated: false, 
     utilPercent: 30.0, memoryBytes: 2GB, engines: {"3D"}},
    {gpuId: "1", gpuName: "Intel UHD 770", isIntegrated: true,
     utilPercent: 45.0, memoryBytes: 512MB, engines: {"Compute"}}
};
```

**Rationale:** Process table shows aggregated values for sorting/filtering; tooltips and Process Details panel show per-GPU breakdown for transparency.

### ProcessModel Integration

`ProcessModel` will be extended to:
1. Accept an optional `IGPUProbe` pointer in constructor
2. During `refresh()`, read `readProcessGPUCounters()` and join with process data by PID
3. Populate GPU fields in `ProcessSnapshot` with proper aggregation for multi-GPU processes
4. Maintain per-GPU breakdown in `perGpuUsage` for detailed views

---

## UI Layer Design

### System Overview Panel

**Enhancements to `SystemMetricsPanel`:**

1. **GPU Section** (new):
   - **Smart default**: Show GPU with highest utilization by default
   - **User selection**: Dropdown menu to select specific GPU or "All GPUs"
   - Each GPU subsection shows:
     - **Header**: GPU name with icon (ICON_FA_MICROCHIP) and type badge
       - Integrated GPUs: Display `[Integrated]` badge with distinct color
       - Discrete GPUs: Display `[Discrete]` badge
       - Example: `🔲 NVIDIA GeForce RTX 4090 [Discrete]`
     - **History graph**: Utilization % over time (consistent with CPU/Memory graphs)
     - **"Now bars"** to the right (vertical layout):
       - GPU Utilization % (with theme color `charts.gpu`)
       - Memory Utilization % (with theme color `charts.gpu_memory`)
       - Temperature °C (with theme color `charts.gpu_temperature`)
       - Power Draw W (if available, with theme color `charts.gpu_power`)
       - Encoder/Decoder % (if available, muted theme color)
   - Layout: Horizontal split (graph left, bars right) matching CPU/Memory sections
   - Uses `ImPlot` for graphs with theme colors

2. **Multi-GPU Layout** (Stacked):
   - **Default Mode**: Stacked vertically with collapsible headers per GPU
   - **Initial State**: GPU with highest utilization expanded, others collapsed
   - **User Control**: Click header to expand/collapse individual GPUs; "Expand All" / "Collapse All" buttons
   - **Layout**:
     ```
     ┌─────────────────────────────────────────────────┐
     │ ▼ GPU 0: NVIDIA RTX 4090 [Discrete] (75%)      │ ← Expanded (highest util)
     │   [Graph + Now Bars]                            │
     ├─────────────────────────────────────────────────┤
     │ ▶ GPU 1: Intel UHD 770 [Integrated] (12%)      │ ← Collapsed
     └─────────────────────────────────────────────────┘
     ```
   - **Persistence**: Save expand/collapse state to config file
   - **Rationale**: Maximizes visible data for primary GPU while allowing access to all GPUs

3. **Missing SDK Guidance**:
   - When vendor SDK not installed, show info banner:
     - Icon: ICON_FA_INFO_CIRCLE (theme color `semantic.text_info`)
     - Message: "Install NVIDIA drivers for enhanced GPU metrics"
     - Button: "Learn More" → opens URL to vendor download page
   - Banner dismissible per session (don't nag)

### Process Table Columns

**New columns in `ProcessPanel`:**

| Column | Description | Format | Notes |
|--------|-------------|--------|-------|
| **GPU %** | GPU utilization | `12.3%` | Aggregate if multi-GPU (see below) |
| **GPU Mem** | GPU memory allocated | `1.2 GB` | Sum across all GPUs |
| **GPU Engine** | Active GPU engines | `3D, Compute` | Union of engines across GPUs |
| **GPU Device** | Which GPU(s) used | `GPU 0` or `0,1` | Show if >1 GPU; aggregate format |

**Multi-GPU Process Aggregation Logic:**
- **GPU %**: If process uses multiple GPUs, show total utilization across all GPUs (e.g., 30% on GPU0 + 45% on GPU1 = 75% total)
- **GPU Mem**: Sum of memory allocated on all GPUs
- **GPU Engine**: Union set of active engines (e.g., "3D" on GPU0, "Compute" on GPU1 → show "3D, Compute")
- **GPU Device**: Show comma-separated list if >1 GPU used (e.g., "0,1" means process uses GPU 0 and GPU 1)
- **Tooltip**: On hover, show per-GPU breakdown:
  ```
  GPU 0 (NVIDIA RTX 4090): 30% util, 2.1 GB, 3D
  GPU 1 (Intel UHD 770): 45% util, 512 MB, Compute
  ```

**Column visibility:** Controllable via right-click menu, persisted to config (existing pattern).

**Capability-based hiding:** If `hasPerProcessMetrics == false`, hide GPU columns.

**Icon usage:**
- GPU column header icon: ICON_FA_MICROCHIP
- Integrated GPU badge in tooltips: ICON_FA_MICROCHIP with muted color
- Discrete GPU badge in tooltips: ICON_FA_MICROCHIP with accent color

### Process Details Panel

**New "GPU" tab in `ProcessDetailsPanel`:**

1. **Overview Section**:
   - GPU Utilization % (text + small sparkline, theme color `charts.gpu`)
   - GPU Memory (text + bar chart showing used/total, theme color `charts.gpu_memory`)
   - Active Engines (badges with theme `accents` colors):
     - "3D" badge (blue accent)
     - "Compute" badge (green accent)
     - "Video Encode" badge (orange accent)
     - "Video Decode" badge (purple accent)
     - "Copy" badge (teal accent)
   - GPU Device (if multi-GPU):
     - Show per-GPU breakdown with collapsible sections
     - Each GPU labeled with icon, name, and type badge

2. **History Graphs** (similar to CPU/Memory tabs):
   - GPU Utilization over time (theme color `charts.gpu`)
   - GPU Memory over time (theme color `charts.gpu_memory`)
   - Encoder/Decoder over time (if available, muted theme colors)
   - If multi-GPU: Stacked line graph showing all GPUs with different accent colors

3. **"Now Bars"** (right side):
   - Current GPU % (theme color `charts.gpu`)
   - Current GPU Memory (theme color `charts.gpu_memory`)
   - Current Encoder % (if available, theme color `charts.gpu_encoder`)
   - Current Decoder % (if available, theme color `charts.gpu_decoder`)
   - Temperature °C (if available, theme color `charts.gpu_temperature`)

**Layout:** Consistent with existing Process Details tabs (Overview, CPU, Memory, Disk).

**Integrated vs Discrete Differentiation:**
- Integrated GPUs show badge icon: ICON_FA_MICROCHIP with `[I]` suffix
- Discrete GPUs show badge icon: ICON_FA_MICROCHIP with `[D]` suffix
- Tooltips explain: "Integrated GPU (shares system memory)" vs "Discrete GPU (dedicated VRAM)"

---

## Windows Implementation Plan

### Technology Stack

| Component | Library/API | Purpose |
|-----------|-------------|---------|
| **System-level metrics** | DXGI (IDXGIAdapter, IDXGIFactory) | GPU enumeration, memory, adapter info |
| **Per-process attribution** | D3DKMT (D3DKMTQueryStatistics) | Per-process GPU usage, engine usage |
| **NVIDIA-specific** | NVML (nvml.h) | Enhanced NVIDIA metrics (temp, power, clocks) |
| **Temperature fallback** | WMI (MSAcpi_ThermalZoneTemperature) | Fallback if NVML unavailable |

### DXGI Probe (`DXGIGPUProbe`)

**Purpose:** System-level GPU metrics for all vendors.

**APIs Used:**
- `CreateDXGIFactory1()` → `IDXGIFactory`
- `EnumAdapters1()` → `IDXGIAdapter1`
- `QueryVideoMemoryInfo()` → VRAM usage
- `GetDesc1()` → GPU name, PCI IDs

**Limitations:**
- No per-process attribution
- No temperature or power metrics
- No clock speeds

### D3DKMT Probe (`D3DKMTGPUProbe`)

**Purpose:** Per-process GPU usage and engine tracking.

**APIs Used:**
- `D3DKMTQueryStatistics()` with `D3DKMT_QUERYSTATISTICS_PROCESS` type
- Provides:
  - Per-process GPU memory allocations
  - GPU node (engine) usage per process
  - Running time per engine

**Engine Mapping:**
```cpp
enum class GPUEngine {
    Graphics3D,    // D3DKMT_NODEMETADATA_TYPE_3D
    Compute,       // D3DKMT_NODEMETADATA_TYPE_COMPUTE
    Copy,          // D3DKMT_NODEMETADATA_TYPE_COPY
    VideoDecode,   // D3DKMT_NODEMETADATA_TYPE_VLD
    VideoEncode,   // D3DKMT_NODEMETADATA_TYPE_VIDEO_ENCODE
};
```

### NVML Probe (`NVMLGPUProbe` - Windows)

**Purpose:** Enhanced metrics for NVIDIA GPUs.

**APIs Used:**
- `nvmlInit_v2()`
- `nvmlDeviceGetHandleByIndex_v2()`
- `nvmlDeviceGetUtilizationRates()` → GPU/Memory utilization
- `nvmlDeviceGetTemperature()` → Temperature
- `nvmlDeviceGetPowerUsage()` → Power draw
- `nvmlDeviceGetClockInfo()` → Clock speeds
- `nvmlDeviceGetComputeRunningProcesses()` → Per-process GPU memory

**Optional:** Link dynamically (`LoadLibrary`) to avoid hard dependency.

### Implementation Steps (Windows)

1. **Phase 1**: DXGI system-level metrics (all vendors)
2. **Phase 2**: D3DKMT per-process metrics
3. **Phase 3**: NVML integration for NVIDIA enhancements
4. **Phase 4**: AMD-specific enhancements (ADL SDK, if feasible)

---

## Linux Implementation Plan

### Technology Stack

| Component | Library/API | Purpose |
|-----------|-------------|---------|
| **NVIDIA** | NVML (libnvidia-ml.so) | System and per-process metrics |
| **AMD** | ROCm (rocm_smi_lib.h) or sysfs | System-level metrics, temperature |
| **Intel/Fallback** | DRM (libdrm) + sysfs | Basic metrics, memory, frequency |
| **Temperature** | hwmon sysfs (`/sys/class/hwmon/`) | GPU temperature for all vendors |

### NVML Probe (`NVMLGPUProbe` - Linux)

**Same as Windows**, with additional Linux-specific features:
- `nvmlDeviceGetProcessUtilization()` → Per-process GPU usage
- `nvmlDeviceGetPcieThroughput()` → PCIe bandwidth

### ROCm Probe (`ROCmGPUProbe`)

**Purpose:** AMD GPU metrics on Linux.

**APIs Used:**
- `rsmi_init()` → Initialize ROCm SMI
- `rsmi_dev_gpu_busy_percent_get()` → GPU utilization
- `rsmi_dev_memory_usage_get()` → VRAM usage
- `rsmi_dev_temp_metric_get()` → Temperature
- `rsmi_dev_power_ave_get()` → Power draw

**Limitations:**
- No per-process GPU usage (ROCm lacks this)
- Fallback: Use `/sys/kernel/debug/dri/[card]/amdgpu_pm_info` (requires root)

### DRM Probe (`DRMGPUProbe`)

**Purpose:** Fallback for Intel integrated GPUs and non-vendor-specific metrics.

**APIs Used:**
- `libdrm` → GPU enumeration via `/dev/dri/card*`
- sysfs (`/sys/class/drm/card*/device/`) → PCI info, memory
- sysfs (`/sys/class/drm/card*/gt_cur_freq_mhz`) → Current GPU frequency
- hwmon (`/sys/class/hwmon/hwmon*/temp*_input`) → Temperature

**Limitations:**
- No per-process metrics
- Limited engine utilization tracking

### Implementation Steps (Linux)

1. **Phase 1**: NVML probe (NVIDIA systems)
2. **Phase 2**: ROCm probe (AMD systems)
3. **Phase 3**: DRM probe (Intel, fallback)
4. **Phase 4**: Composite probe that tries all three

---

## Library Dependencies and CMake Integration

### Dependencies Summary

| Library | Platform | Purpose | License | Linking |
|---------|----------|---------|---------|---------|
| **NVML** | Windows, Linux | NVIDIA GPU metrics | Proprietary (redistributable) | Dynamic (optional) |
| **ROCm SMI** | Linux | AMD GPU metrics | MIT | Dynamic (optional) |
| **libdrm** | Linux | DRM/KMS GPU access | MIT | Dynamic (system) |
| **DXGI** | Windows | Direct3D GPU info | Microsoft (system) | Static (system) |
| **D3DKMT** | Windows | Kernel-mode thunks | Microsoft (system) | Static (system) |

### CMake Integration

#### 1. Feature Flag

```cmake
option(TASKSMACK_ENABLE_GPU_SUPPORT "Enable GPU monitoring support" ON)
```

#### 2. Conditional Compilation

```cmake
if(TASKSMACK_ENABLE_GPU_SUPPORT)
    # Platform-specific sources
    if(WIN32)
        list(APPEND TASKSMACK_SOURCES
            src/Platform/Windows/WindowsGPUProbe.cpp
            src/Platform/Windows/DXGIGPUProbe.cpp
            src/Platform/Windows/D3DKMTGPUProbe.cpp
            src/Platform/Windows/NVMLGPUProbe.cpp
        )
        # Link Windows system libraries
        target_link_libraries(TaskSmack PRIVATE dxgi d3d11)
    elseif(UNIX AND NOT APPLE)
        list(APPEND TASKSMACK_SOURCES
            src/Platform/Linux/LinuxGPUProbe.cpp
            src/Platform/Linux/NVMLGPUProbe.cpp
            src/Platform/Linux/ROCmGPUProbe.cpp
            src/Platform/Linux/DRMGPUProbe.cpp
        )
        # Optional: Find libdrm (system package)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBDRM libdrm)
            if(LIBDRM_FOUND)
                target_include_directories(TaskSmack PRIVATE ${LIBDRM_INCLUDE_DIRS})
                target_link_libraries(TaskSmack PRIVATE ${LIBDRM_LIBRARIES})
            endif()
        endif()
    endif()

    # Domain layer
    list(APPEND TASKSMACK_SOURCES
        src/Domain/GPUModel.cpp
    )

    # Compile definition
    target_compile_definitions(TaskSmack PRIVATE TASKSMACK_GPU_SUPPORT=1)
endif()
```

#### 3. NVML Integration (Static Linking Preferred)

```cmake
# NVML: Static link if possible, dynamic fallback
if(WIN32)
    # Windows: Look for CUDA toolkit or standalone NVML
    find_path(NVML_INCLUDE_DIR nvml.h
        PATHS
            "$ENV{CUDA_PATH}/include"
            "$ENV{ProgramFiles}/NVIDIA GPU Computing Toolkit/CUDA/*/include"
    )
    find_library(NVML_LIBRARY nvml
        PATHS
            "$ENV{CUDA_PATH}/lib/x64"
            "$ENV{ProgramFiles}/NVIDIA GPU Computing Toolkit/CUDA/*/lib/x64"
    )
    if(NVML_INCLUDE_DIR AND NVML_LIBRARY)
        target_include_directories(TaskSmack PRIVATE ${NVML_INCLUDE_DIR})
        target_link_libraries(TaskSmack PRIVATE ${NVML_LIBRARY})
        target_compile_definitions(TaskSmack PRIVATE TASKSMACK_HAVE_NVML=1 TASKSMACK_NVML_STATIC=1)
        message(STATUS "NVML: Static linking enabled (${NVML_LIBRARY})")
    elseif(NVML_INCLUDE_DIR)
        # Headers found but not library: use dynamic loading fallback
        target_include_directories(TaskSmack PRIVATE ${NVML_INCLUDE_DIR})
        target_compile_definitions(TaskSmack PRIVATE TASKSMACK_HAVE_NVML=1)
        message(STATUS "NVML: Dynamic loading fallback (headers only)")
    else()
        message(STATUS "NVML: Not found, NVIDIA GPU support will be limited")
    endif()
else()
    # Linux: Prefer static linking, fallback to dynamic
    find_path(NVML_INCLUDE_DIR nvml.h
        PATHS /usr/include/nvidia/gdk /usr/local/cuda/include
    )
    find_library(NVML_LIBRARY nvidia-ml
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/local/cuda/lib64
    )
    if(NVML_INCLUDE_DIR AND NVML_LIBRARY)
        target_include_directories(TaskSmack PRIVATE ${NVML_INCLUDE_DIR})
        target_link_libraries(TaskSmack PRIVATE ${NVML_LIBRARY})
        target_compile_definitions(TaskSmack PRIVATE TASKSMACK_HAVE_NVML=1 TASKSMACK_NVML_STATIC=1)
        message(STATUS "NVML: Static linking enabled (${NVML_LIBRARY})")
    elseif(NVML_INCLUDE_DIR)
        # Headers found but not library: use dynamic loading fallback
        target_include_directories(TaskSmack PRIVATE ${NVML_INCLUDE_DIR})
        target_compile_definitions(TaskSmack PRIVATE TASKSMACK_HAVE_NVML=1)
        message(STATUS "NVML: Dynamic loading fallback (headers only)")
    else()
        message(STATUS "NVML: Not found, NVIDIA GPU support will be limited")
    endif()
endif()

# Minimum NVML version check (API 11+, Driver 450+)
# This is enforced at runtime in NVMLGPUProbe::initialize()
```

**Rationale:** Static linking preferred per maintainer. Dynamic loading is fallback when static library unavailable (e.g., user has drivers but not SDK).

#### 4. Optional ROCm Integration (Dynamic Loading)

```cmake
if(UNIX AND NOT APPLE)
    find_path(ROCM_SMI_INCLUDE_DIR rocm_smi/rocm_smi.h
        PATHS /opt/rocm/include
    )
    find_library(ROCM_SMI_LIBRARY rocm_smi64
        PATHS /opt/rocm/lib
    )
    if(ROCM_SMI_INCLUDE_DIR AND ROCM_SMI_LIBRARY)
        target_include_directories(TaskSmack PRIVATE ${ROCM_SMI_INCLUDE_DIR})
        target_link_libraries(TaskSmack PRIVATE ${ROCM_SMI_LIBRARY})
        target_compile_definitions(TaskSmack PRIVATE TASKSMACK_HAVE_ROCM=1)
    endif()
endif()
```

### Library Loading Strategy

**Static Linking (Preferred):**
- NVML: Static link when SDK available
- DXGI/D3DKMT: Static link (Windows system libraries)
- libdrm: Static link (Linux system library)
- ROCm: Dynamic loading only (optional, late priority)

**Dynamic Loading (Fallback):**
When static library unavailable but headers present:

1. **Define function pointers** for all vendor API functions
2. **At runtime**, attempt `dlopen("libnvidia-ml.so")` (Linux) or `LoadLibrary("nvml.dll")` (Windows)
3. **Resolve symbols** using `dlsym()` or `GetProcAddress()`
4. **If loading fails**, disable vendor features and log warning
5. **Fallback** to DXGI (Windows) or DRM (Linux)

**Example (when TASKSMACK_NVML_STATIC not defined):**
```cpp
// In NVMLGPUProbe.cpp
bool NVMLGPUProbe::initialize() {
#ifdef TASKSMACK_NVML_STATIC
    // Use statically linked functions directly
    nvmlReturn_t result = nvmlInit_v2();
    if (result != NVML_SUCCESS) {
        spdlog::warn("NVML initialization failed: {}", nvmlErrorString(result));
        return false;
    }
#else
    // Dynamic loading fallback
#ifdef _WIN32
    m_NVMLHandle = LoadLibraryA("nvml.dll");
#else
    m_NVMLHandle = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
#endif
    if (!m_NVMLHandle) {
        spdlog::warn("NVML library not found, NVIDIA GPU support disabled");
        return false;
    }
    // Load function pointers...
    auto nvmlInit = reinterpret_cast<nvmlReturn_t(*)()>(
        dlsym(m_NVMLHandle, "nvmlInit_v2"));
    if (!nvmlInit) {
        spdlog::warn("NVML functions not found");
        return false;
    }
#endif
    
    // Check driver version (require NVML 11+, Driver 450+)
    nvmlReturn_t result = nvmlInit_v2();
    if (result != NVML_SUCCESS) {
        spdlog::warn("NVML init failed: Update NVIDIA drivers to 450+ for GPU monitoring");
        return false;
    }
    return true;
}
```

### Build Script Changes

**Update `tools/` scripts:**
- **`check-prereqs.sh` / `check-prereqs.ps1`**: Check for optional GPU libraries and SDK availability
- Document in CONTRIBUTING.md that GPU support has tiered levels:
  - **Tier 1 (Full):** NVML SDK installed (static linking)
  - **Tier 2 (Runtime):** NVIDIA drivers only (dynamic loading)
  - **Tier 3 (Basic):** No vendor SDKs (DXGI/DRM fallback)

### Theme Integration

**New GPU-specific colors to add to theme files** (e.g., `assets/themes/windows-dark.toml`):

```toml
# =============================================================================
# GPU METRICS COLORS
# Colors for GPU monitoring graphs, bars, and badges
# =============================================================================
[charts.gpu]
utilization = "#8E8CD8"           # Purple - GPU utilization line
utilization_fill = "#8E8CD84D"    # Purple with 30% alpha - GPU fill
memory = "#E74856"                # Red - GPU memory line
memory_fill = "#E748564D"         # Red with 30% alpha - GPU memory fill
temperature = "#FFB900"           # Gold - GPU temperature line
temperature_fill = "#FFB9004D"    # Gold with 30% alpha - GPU temp fill
power = "#10893E"                 # Green - GPU power draw line
power_fill = "#10893E4D"          # Green with 30% alpha - GPU power fill
encoder = "#00B7C3"               # Teal - GPU encoder utilization
decoder = "#E3008C"               # Magenta - GPU decoder utilization

# GPU type badges
integrated_badge = "#8A8A8A"      # Muted gray - integrated GPU badge
discrete_badge = "#0078D4"        # Windows Blue - discrete GPU badge

# GPU engine badges (uses existing accent colors, documented here for clarity)
# engine_3d = accents.colors[0]        # Blue - 3D engine
# engine_compute = accents.colors[2]   # Green - Compute engine
# engine_encode = accents.colors[4]    # Orange - Video encode
# engine_decode = accents.colors[3]    # Purple - Video decode
# engine_copy = accents.colors[5]      # Teal - Copy engine
```

**Icon Constants** (already available in `src/UI/IconsFontAwesome6.h`):
- `ICON_FA_MICROCHIP`: Primary GPU icon for headers, columns, badges
- `ICON_FA_INFO_CIRCLE`: For installation guidance banners
- `ICON_FA_COMPUTER`: Alternative GPU/system icon

**Theme Color Access Pattern** (consistent with existing code):
```cpp
// In SystemMetricsPanel.cpp
const auto& theme = UI::Theme::getCurrent();
const auto gpuColor = theme.charts.gpu.utilization;
const auto gpuFillColor = theme.charts.gpu.utilization_fill;

// For integrated vs discrete badges
const auto badgeColor = gpuInfo.isIntegrated 
    ? theme.charts.gpu.integrated_badge 
    : theme.charts.gpu.discrete_badge;
```

**All existing themes must be updated** to include the new `[charts.gpu]` section. Themes without this section will use sensible defaults (fallback to existing accent colors).

---

## Testing Strategy

### Unit Tests

**Domain Layer (`tests/Domain/test_GPUModel.cpp`):**

1. **Mock GPU Probe** (`MockGPUProbe`):
   - Fluent API: `withGPU("GPU0", "NVIDIA RTX 4090").withUtilization("GPU0", 75.0)`
   - Test delta calculation for PCIe throughput rates
   - Test multi-GPU scenarios (0, 1, 2+ GPUs)
   - Test capability reporting and graceful degradation

2. **Test Cases**:
   - Single GPU utilization tracking
   - Multi-GPU utilization tracking
   - History buffer correctness (GPU snapshots over time)
   - Rate calculations (PCIe bytes/sec)
   - Process-GPU association (joining process data with GPU data)
   - Capability-based degradation (e.g., no temperature sensor)

**Platform Layer (`tests/Platform/test_GPUProbe.cpp`):**

1. **Contract Tests** (Linux/Windows):
   - `enumerateGPUs()` returns valid GPU info
   - `readGPUCounters()` returns consistent data
   - Capabilities match actual hardware
   - Invalid GPU ID handling

2. **Vendor-Specific Tests**:
   - NVML initialization (if available)
   - ROCm initialization (if available)
   - DXGI enumeration on Windows
   - D3DKMT per-process queries on Windows

### Integration Tests

**Full Stack (`tests/Integration/test_GPUIntegration.cpp`):**

1. **Scenario**: System with GPU detected
   - Create `GPUModel` with real probe
   - Call `refresh()` multiple times
   - Verify snapshots are populated
   - Verify history grows correctly

2. **Scenario**: System without GPU
   - `enumerateGPUs()` returns empty vector
   - UI gracefully hides GPU sections

3. **Scenario**: Multi-GPU system
   - Verify all GPUs enumerated
   - Verify independent history per GPU

### Manual Testing

**Test Matrix:**

| Platform | GPU | Test Case |
|----------|-----|-----------|
| Windows 11 | NVIDIA RTX | System metrics, per-process attribution, NVML features |
| Windows 11 | AMD Radeon | System metrics via DXGI, per-process via D3DKMT |
| Windows 11 | Intel iGPU | System metrics via DXGI |
| Ubuntu 24.04 | NVIDIA RTX | NVML metrics, per-process GPU usage |
| Ubuntu 24.04 | AMD Radeon | ROCm metrics, sysfs fallback |
| Ubuntu 24.04 | Intel iGPU | DRM metrics, sysfs temperature |
| VM (no GPU) | None | Graceful degradation, no GPU section shown |

**UI Verification:**
- System Overview: GPU graphs render correctly
- Process Table: GPU columns show accurate data
- Process Details: GPU tab shows history and "now bars"
- Multi-GPU: All GPUs visible and switchable

---

## Implementation Phases

**Vendor Priority:** NVIDIA → Intel → AMD (per maintainer guidance)

### Phase 1: Foundation (Weeks 1-2)

**Goals:**
- Platform layer interfaces and types
- Domain layer `GPUModel` skeleton
- Factory integration
- Mock probes for testing

**Deliverables:**
- [ ] `src/Platform/GPUTypes.h` with raw counter structs
- [ ] `src/Platform/IGPUProbe.h` interface
- [ ] `src/Domain/GPUSnapshot.h` and `GPUModel.h`
- [ ] `tests/Mocks/MockGPUProbe.h`
- [ ] Unit tests for `GPUModel` with mock probe
- [ ] CMake integration (feature flag, conditional compilation)
- [ ] Theme file updates with GPU colors

### Phase 2: NVIDIA Support - Windows (Weeks 3-4)

**Goals:**
- DXGI probe for basic Windows GPU enumeration (all vendors)
- NVML probe for enhanced NVIDIA metrics
- System Overview UI integration
- **Priority:** NVIDIA first per maintainer request

**Deliverables:**
- [ ] `src/Platform/Windows/DXGIGPUProbe.cpp` (basic enumeration)
- [ ] `src/Platform/Windows/NVMLGPUProbe.cpp` (NVIDIA-specific)
- [ ] `src/Platform/Windows/WindowsGPUProbe.cpp` (composite)
- [ ] `src/App/Panels/SystemMetricsPanel.cpp` extended with GPU section
- [ ] Static linking for NVML where possible, dynamic fallback
- [ ] Integration tests on Windows with NVIDIA GPU
- [ ] Driver version check (NVML 11+, Driver 450+)

### Phase 3: NVIDIA Support - Windows Per-Process (Weeks 5-6)

**Goals:**
- D3DKMT probe for per-process GPU usage (all vendors via Windows API)
- NVML per-process enhancements for NVIDIA
- Process table GPU columns
- Process Details GPU tab

**Deliverables:**
- [ ] `src/Platform/Windows/D3DKMTGPUProbe.cpp`
- [ ] `src/Domain/ProcessSnapshot.h` extended with GPU fields
- [ ] `src/Domain/ProcessModel.cpp` integration with GPU probe
- [ ] `src/App/Panels/ProcessPanel.cpp` GPU columns
- [ ] `src/App/Panels/ProcessDetailsPanel.cpp` GPU tab
- [ ] Multi-GPU aggregation logic in ProcessModel
- [ ] Unit tests for process-GPU association

### Phase 4: NVIDIA Support - Linux (Weeks 7-8)

**Goals:**
- NVML probe for Linux (NVIDIA)
- System and per-process metrics
- Complete NVIDIA cross-platform support

**Deliverables:**
- [ ] `src/Platform/Linux/NVMLGPUProbe.cpp` (shared code with Windows where possible)
- [ ] Static linking for NVML, dynamic fallback
- [ ] Integration tests on Linux with NVIDIA GPU
- [ ] Documentation for Linux NVIDIA driver requirements
- [ ] User guidance banner when NVIDIA drivers not installed

### Phase 5: Intel Support (Weeks 9-10)

**Goals:**
- Intel GPU support on both Windows and Linux
- DRM probe (Linux) for Intel integrated GPUs
- DXGI fallback (Windows) for Intel GPUs
- **Priority:** Intel second per maintainer request

**Deliverables:**
- [ ] `src/Platform/Linux/DRMGPUProbe.cpp` (Intel-focused)
- [ ] Intel GPU detection and integrated vs discrete labeling
- [ ] Basic metrics via DRM/sysfs (Linux) and DXGI (Windows)
- [ ] Integration tests on systems with Intel integrated GPUs
- [ ] Temperature monitoring via hwmon (Linux)

### Phase 6: AMD Support (Weeks 11-12)

**Goals:**
- AMD GPU support on both Windows and Linux
- ROCm probe (Linux) for AMD discrete GPUs
- DXGI fallback (Windows) for AMD GPUs
- **Priority:** AMD last per maintainer request

**Deliverables:**
- [ ] `src/Platform/Linux/ROCmGPUProbe.cpp`
- [ ] `src/Platform/Linux/LinuxGPUProbe.cpp` (composite: NVML → DRM → ROCm)
- [ ] Dynamic loading for ROCm library (optional dependency)
- [ ] Integration tests on Linux with AMD GPUs
- [ ] ROCm 5.0+ requirement, user guidance for installation
- [ ] Fallback behavior when ROCm unavailable

### Phase 7: Polishing and Documentation (Week 13)

**Goals:**
- Multi-GPU UI refinements (stacked layout)
- Performance optimization
- Comprehensive documentation
- Close related issues

**Deliverables:**
- [ ] Multi-GPU stacked UI in System Overview (collapsible headers)
- [ ] Column visibility persistence for GPU columns
- [ ] Performance profiling and optimization
- [ ] Update `tasksmack.md` with GPU architecture
- [ ] Update `completed-features.md`
- [ ] Update all theme files with GPU color sections
- [ ] Close issues #178, #189, #176
- [ ] Create user guide for GPU monitoring features
- [ ] Installation guidance documentation for NVML, ROCm, intel-gpu-tools

---
- [ ] Column visibility persistence for GPU columns
- [ ] Performance profiling and optimization
- [ ] Update `tasksmack.md` with GPU architecture
- [ ] Update `completed-features.md`
- [ ] Close issues #178, #189, #176
- [ ] Create user guide for GPU monitoring features

---

## UI Design Comparisons with Other Applications

To ensure TaskSmack's GPU monitoring is intuitive and competitive, we compare approaches with established applications:

### Windows Task Manager (Reference)

**System Overview:**
- Shows all GPUs in Performance tab with individual graphs
- Each GPU shows: Utilization, Dedicated Memory, Shared Memory, Clock Speed, Temperature
- GPU selection via left sidebar (click GPU 0, GPU 1, etc.)
- "Now bars" on right show current values

**Process List:**
- Columns: GPU, GPU Engine, Dedicated GPU Memory
- GPU column shows which GPU is primary for that process
- GPU Engine shows active engines (e.g., "Copy", "3D", "Compute")

**TaskSmack Approach:**
- ✅ Match: System overview graphs and "now bars" similar
- ✅ Improve: Smart default (auto-select highest-usage GPU) vs manual selection
- ✅ Match: Process list GPU columns similar
- ✅ Improve: Multi-GPU aggregation (Task Manager only shows primary GPU)

### htop / btop++ (Reference)

**GPU Support in btop++:**
- Shows all GPUs in system overview
- Per-GPU graphs stacked vertically
- GPU process list shows which processes use GPU
- Limited to basic metrics (utilization, memory)

**TaskSmack Approach:**
- ✅ Match: Stacked GPU graphs similar to btop++
- ✅ Improve: Per-process engine attribution (btop++ lacks this)
- ✅ Improve: Detailed per-GPU breakdown in process details
- ✅ Match: Support for AMD/Intel GPUs (btop++ limited to NVIDIA on Linux)

### NVIDIA System Monitor / AMD Radeon Software (Vendor Tools)

**NVIDIA-specific features:**
- Per-process GPU usage (via nvidia-smi)
- Engine-specific utilization (SM, Copy, Encoder, Decoder)
- Temperature, power, clock speeds

**TaskSmack Approach:**
- ✅ Match: Same metrics via NVML
- ✅ Improve: Unified UI for all vendors (not vendor-specific)
- ✅ Match: Per-process attribution
- ⚠️ Limitation: AMD lacks per-process on Linux (vendor limitation)

### TaskSmack Design Philosophy

**Key differentiators:**
1. **Vendor-neutral**: Same UI for NVIDIA, AMD, Intel (unlike vendor tools)
2. **Smart defaults**: Auto-show GPU with highest usage (unlike Task Manager)
3. **Multi-GPU aggregation**: Process table shows total across all GPUs (Task Manager shows primary only)
4. **Guided installation**: Prompts to install vendor SDKs (unique to TaskSmack)
5. **Integrated vs Discrete awareness**: Clear labeling and badges (not common in other tools)

**Alignment with established patterns:**
- Graph layout: Similar to Task Manager and btop++
- Column naming: Follows Task Manager conventions (GPU %, GPU Engine)
- "Now bars": Consistent with TaskSmack's existing CPU/Memory panels

---

## Security and Performance Considerations

### Security

1. **Privileged Operations**:
   - Windows D3DKMT: No special privileges required
   - Linux `/sys/kernel/debug/dri`: Requires root or CAP_SYS_ADMIN
   - **Mitigation**: Fail gracefully if access denied; log warning

2. **Dynamic Library Loading**:
   - NVML and ROCm loaded dynamically to avoid hard dependency
   - Validate library paths to prevent DLL injection
   - **Mitigation**: Use absolute paths or system library directories only

3. **Input Validation**:
   - Sanitize GPU ID strings from vendor APIs
   - Validate array bounds for multi-GPU indices

4. **Denial of Service**:
   - Vendor APIs may hang or be slow
   - **Mitigation**: Use timeouts for probe calls; offload to sampler thread

### Performance

1. **Sampling Frequency**:
   - GPU probes called at same interval as CPU/memory (default 1 second)
   - Configurable via `SamplingConfig.h`

2. **API Overhead**:
   - NVML calls: ~100-200 μs per call (negligible)
   - DXGI `QueryVideoMemoryInfo()`: ~500 μs
   - D3DKMT `QueryStatistics()`: ~1-2 ms (can be slow with many processes)
   - **Optimization**: Cache GPU enumeration; only query counters on refresh

3. **Memory Overhead**:
   - Each GPU adds ~2 KB per sample to history buffer
   - For 60-second history at 1 Hz: 120 KB per GPU
   - Multi-GPU (4 GPUs): ~480 KB (acceptable)

4. **Thread Safety**:
   - `GPUModel` uses `std::shared_mutex` (same pattern as `ProcessModel`)
   - Vendor APIs are generally thread-safe, but document any exceptions

5. **Graceful Degradation**:
   - If GPU enumeration takes >5 seconds, warn and disable GPU support
   - If probe fails multiple times consecutively, stop polling and notify user

---

## Future Enhancements

### Post-MVP Features

1. **GPU Process Tree**:
   - Group processes by GPU device in process table
   - Tree view showing which processes use which GPU

2. **GPU Alerts**:
   - Threshold alerts (e.g., "GPU memory >90%")
   - Temperature warnings (e.g., "GPU >85°C")

3. **Historical Metrics Export**:
   - Export GPU metrics to CSV/JSON
   - Integration with external monitoring (Prometheus, Grafana)

4. **Remote GPU Monitoring**:
   - Expose GPU metrics via HTTP API (read-only)
   - Useful for headless servers with GPUs

5. **GPU Profiling**:
   - Per-shader/kernel GPU time (requires vendor tools: NSight, Radeon GPU Profiler)
   - GPU memory bandwidth utilization

6. **Power Management**:
   - Show P-state (performance state) of GPU
   - Show throttling reasons (thermal, power limit)

7. **VRAM Allocation Breakdown**:
   - Show which processes are using which memory types (dedicated, shared, system)

8. **AMD ADL Integration** (Windows):
   - AMD Display Library for enhanced AMD metrics on Windows
   - More detailed than DXGI, less than ROCm

9. **Intel Level Zero Support**:
   - Intel's oneAPI Level Zero for compute-focused metrics
   - Useful for data center GPUs (Intel Xe)

### Research Areas

1. **macOS Metal Support**:
   - Use Metal Performance Shaders to query GPU metrics
   - Apple Silicon GPU monitoring

2. **Container GPU Isolation**:
   - Track GPU usage per container (Docker, Kubernetes)
   - Requires integration with container runtimes

**Note:** Virtualized GPU (vGPU) monitoring is explicitly out of scope per maintainer guidance. TaskSmack focuses on bare metal GPU monitoring only.

---

## Appendix: Vendor API References

### NVIDIA NVML

**Documentation**: [NVML API Reference](https://docs.nvidia.com/deploy/nvml-api/index.html)

**Key Functions**:
- `nvmlInit_v2()` / `nvmlShutdown()`
- `nvmlDeviceGetCount_v2()`
- `nvmlDeviceGetHandleByIndex_v2()`
- `nvmlDeviceGetName()`
- `nvmlDeviceGetUtilizationRates()`
- `nvmlDeviceGetMemoryInfo()`
- `nvmlDeviceGetTemperature()`
- `nvmlDeviceGetPowerUsage()`
- `nvmlDeviceGetClockInfo()`
- `nvmlDeviceGetComputeRunningProcesses_v3()`
- `nvmlDeviceGetProcessUtilization()` (Linux)

### AMD ROCm SMI

**Documentation**: [ROCm SMI Library](https://github.com/RadeonOpenCompute/rocm_smi_lib)

**Key Functions**:
- `rsmi_init()` / `rsmi_shut_down()`
- `rsmi_num_monitor_devices()`
- `rsmi_dev_gpu_busy_percent_get()`
- `rsmi_dev_memory_usage_get()`
- `rsmi_dev_temp_metric_get()`
- `rsmi_dev_power_ave_get()`
- `rsmi_dev_overdrive_level_get()`

### Windows DXGI

**Documentation**: [DXGI Overview (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)

**Key Interfaces**:
- `IDXGIFactory::EnumAdapters()`
- `IDXGIAdapter::GetDesc()` / `GetDesc1()`
- `IDXGIAdapter3::QueryVideoMemoryInfo()`

### Windows D3DKMT

**Documentation**: [D3DKMT Functions (Windows Drivers)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/_display/)

**Key Functions**:
- `D3DKMTEnumAdapters2()`
- `D3DKMTQueryStatistics()`
- Query types: `D3DKMT_QUERYSTATISTICS_PROCESS`, `D3DKMT_QUERYSTATISTICS_ADAPTER`

### Linux DRM

**Documentation**: [Direct Rendering Manager (kernel.org)](https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html)

**Key Files (sysfs)**:
- `/sys/class/drm/card*/device/mem_info_vram_used`
- `/sys/class/drm/card*/gt_cur_freq_mhz`
- `/sys/class/drm/renderD*/device/` (Vulkan device info)

### Linux hwmon

**Documentation**: [Hardware Monitoring Kernel API](https://www.kernel.org/doc/html/latest/hwmon/hwmon-kernel-api.html)

**Key Files**:
- `/sys/class/hwmon/hwmon*/name` (identify device)
- `/sys/class/hwmon/hwmon*/temp*_input` (temperature in millidegrees Celsius)

---

## Closing Notes

### Issues Addressed

This plan addresses the following GitHub issues:

- **Issue #178**: GPU Stats (utilization, memory, temperature)
- **Issue #189**: GPU Engine Column (which GPU engine is in use)
- **Issue #176**: Temperature Sensors (includes GPU temperature)

### Related Documentation

This plan should be linked from:
- `tasksmack.md` → Add "GPU Monitoring" section referencing this document
- `CONTRIBUTING.md` → Add note about GPU libraries being optional dependencies
- `completed-features.md` → Update after implementation is complete

### Maintainability

This design prioritizes:
1. **Testability**: Mock probes enable unit testing without hardware
2. **Extensibility**: New vendors can be added without changing Domain/UI layers
3. **Portability**: Vendor-specific code isolated to Platform layer
4. **Graceful Degradation**: UI adapts to available hardware/capabilities

### Questions for Maintainers

Answers provided by maintainer:

1. **GPU vendor priority**: NVIDIA first, then Intel, then AMD
   - **Implementation order**: Phase 1-3 focus on NVIDIA (Windows/Linux), Phase 4 adds Intel, Phase 5 adds AMD
   - **Justification**: NVIDIA has best API support (NVML) and largest user base

2. **Multi-GPU UI**: Stacked sections in System Overview
   - **Layout**: Vertical stack with collapsible headers per GPU
   - **Default**: Show GPU with highest utilization expanded, others collapsed
   - **Rationale**: Maximizes screen space efficiency, consistent with CPU per-core view

3. **Library linking**: Static linking is preferred where possible
   - **NVML**: Static link if headers available, fallback to dynamic loading
   - **ROCm/ADL**: Dynamic loading (optional dependency)
   - **DXGI/D3DKMT**: Static link (system libraries on Windows)
   - **libdrm**: Static link (system library on Linux)

4. **Minimum driver versions**: Target recent drivers, no legacy support
   - **NVIDIA**: NVML API version 11+ (Driver 450+, released 2020)
   - **AMD**: ROCm 5.0+ (released 2022)
   - **Intel**: Current generation drivers only
   - **Rationale**: Users keep drivers up-to-date; supporting old drivers complicates code and loses features
   - **UI Guidance**: If driver too old, show banner: "Update your GPU drivers to enable monitoring"

5. **Virtualized environments**: Not in scope (bare metal only)
   - **Focus**: Physical GPU monitoring on bare metal systems
   - **vGPU/VM**: Not supported in initial implementation
   - **Future**: May revisit if user demand exists

---

**Document Version:** 2.0  
**Last Updated:** 2025-12-31  
**Author:** GitHub Copilot (Coding Agent)  
**Status:** Reviewed with Maintainer Feedback Incorporated
