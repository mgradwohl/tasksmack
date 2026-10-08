#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

// Capability reporting
struct GPUCapabilities
{
    bool hasTemperature = false;
    bool hasPowerMetrics = false;
    bool hasClockSpeeds = false;
    bool hasFanSpeed = false;
    bool hasEngineUtilization = false;
    bool hasPerProcessMetrics = false; // Per-process GPU usage
    // Of those, per-process utilization (ProcessGPUCounters::gpuUtilPercent). Some backends report a
    // process's GPU memory and engines but not its utilization (NVML's running-process lists), which
    // then reads 0 for every process (#1210).
    bool hasPerProcessUtilization = false;
    bool hasEncoderDecoder = false;
    bool supportsMultiGPU = false;
};

/// How thoroughly IGPUProbe::rescanGPUs() looks for changes to the GPU set (#1116).
enum class GPURescan : std::uint8_t
{
    Quick, ///< Every sample: only what the probe can tell cheaply, without waking a GPU.
    Full,  ///< At a low rate: may also look for added, removed or lost devices and rebuild the device list.
};

/// Where an adapter sits on the PCI bus. DXGI and NVML enumerate adapters in different orders and
/// name them differently, so on Windows this is what says which NVML device is which DXGI adapter
/// (#1091). DXGI reports no PCI domain, so the domain is not part of the match. The function number
/// tells apart two functions of one multi-function device; it is nullopt where the source could not
/// say (NVML's busId string unreadable), and samePciLocation() then matches on bus and device alone.
struct PciLocation
{
    std::uint32_t bus = 0;
    std::uint32_t device = 0;
    std::optional<std::uint32_t> function;
    bool operator==(const PciLocation&) const = default;
};

/// Whether @p a and @p b name the same PCI function: the same bus and device, and the same function
/// number unless either side doesn't know it. An unknown function is treated as a wildcard rather
/// than as function 0, so a source that couldn't read it still matches the adapter at its bus and
/// device; the stricter operator== is for comparing fully read locations.
[[nodiscard]] constexpr bool samePciLocation(const PciLocation& a, const PciLocation& b) noexcept
{
    return a.bus == b.bus && a.device == b.device && (!a.function.has_value() || !b.function.has_value() || *a.function == *b.function);
}

// Identifies a physical GPU
struct GPUInfo
{
    std::string id;     // Stable identifier, unique among present GPUs (Windows: "PCI_01:00.0_10DE:2684", slot + model, #1317)
    std::string luidId; // LUID-based identifier for PDH matching (e.g., "GPU_0x00000000_0x0000F78E")
    std::string name;   // Human-readable name (e.g., "NVIDIA GeForce RTX 2080 Ti")
    std::string vendor; // "NVIDIA", "AMD", "Intel", "Qualcomm" (Windows), "Unknown"
    std::string driverVersion;
    bool isIntegrated = false;     // Integrated vs discrete
    std::uint32_t deviceIndex = 0; // Vendor-specific index (on Windows, DXGI's display order, not identity)
    /// Which memory segment the adapter's used/total figures count: true for its shared segment
    /// (system memory the GPU maps -- a Windows integrated GPU's memory), false for dedicated VRAM
    /// (every discrete GPU, and an APU's carve-out on Linux, where NVML/ROCm SMI/DRM have no shared
    /// segment). Set by the probe that reads the figure, so per-process memory is counted against
    /// the same segment whatever its value -- a 0 shared reading is a reading, not "no segment" (#1164).
    bool memoryIsShared = false;
    /// The sensor metrics this particular adapter reports (temperature, power, clocks, fan,
    /// encoder/decoder); the other fields are not used. GPUCapabilities from a probe
    /// describes the probe as a whole, so on a hybrid Windows laptop NVML's capabilities applied to
    /// the Intel iGPU too, and two NVIDIA cards with different sensors both drew every series
    /// (#1040). nullopt means the probe's capabilities apply to this adapter unchanged. Set on
    /// Windows (from NVML) and on Linux by each vendor probe: NVML and ROCm SMI by which sensor
    /// reads succeed at enumeration, DRM by which sysfs/hwmon files the card has (#1112). A GPU
    /// asleep at enumeration is not woken to find out (#1117): it stays nullopt until the GPU is
    /// first seen awake, when the probe asks GPUModel to re-enumerate and publish it (#1289).
    std::optional<GPUCapabilities> sensorCapabilities;
    /// PCI bus location, where the probe can read it (Windows: DXGI via D3DKMT, and NVML; Linux: NVML) (#1091).
    std::optional<PciLocation> pciLocation;
    /// PCI (device ID << 16) | vendor ID -- NVML's pciDeviceId encoding -- or 0 when unknown (#1091).
    std::uint32_t pciDeviceId = 0;
};

/// The engine classes DRM fdinfo reports busyness for (the kernel's drm-usage-stats.rst): i915 names
/// them render/copy/video/video-enhance/compute, xe rcs/bcs/vcs/vecs/ccs (#1267).
enum class GPUEngineClass : std::uint8_t
{
    Render,
    Copy,
    Video,
    VideoEnhance,
    Compute,
};
inline constexpr std::size_t GPU_ENGINE_CLASS_COUNT = 5;

/// One engine class's cumulative busyness for one DRM client. `busy` and `total` are in one unit, so
/// their changes between two samples give the share of the time the client kept the class busy:
/// i915 reports busy nanoseconds, and the probe stamps `total` with CLOCK_MONOTONIC nanoseconds as
/// it reads them; xe reports busy GPU-timestamp cycles together with the GPU timestamp itself.
struct GPUEngineBusyCounter
{
    bool available = false;
    std::uint64_t busy = 0;
    std::uint64_t total = 0;
    std::uint32_t capacity = 1; // Engines of the class (drm-engine-capacity-*; the kernel omits it when 1)
};

/// One DRM client's (one open DRM file's) cumulative engine busyness, from /proc/<pid>/fdinfo (#1267).
struct GPUEngineClientCounters
{
    std::uint64_t clientId = 0; // drm-client-id: one per open DRM file, shared by dup'd and inherited fds
    std::array<GPUEngineBusyCounter, GPU_ENGINE_CLASS_COUNT> engines{};
};

// Raw GPU counters (Platform layer provides raw values only)
// Derived metrics (rates, percentages) are computed by Domain layer
struct GPUCounters
{
    std::string gpuId; // Associates with GPUInfo

    // Whether this sample's read of each field succeeded. False when a supported sensor couldn't be
    // read this time (NVML_ERROR_TIMEOUT, GPU lost, a driver reset): its value is then meaningless,
    // not a real 0 -- the history records a gap and the bar shows N/A (#1111). Default true, so a
    // probe that never fails a read needn't set them.
    bool utilizationAvailable = true;
    bool temperatureAvailable = true;
    bool powerAvailable = true;
    bool gpuClockAvailable = true;
    bool memoryAvailable = true; // used/total bytes, and so the memory percent
    bool encoderAvailable = true;
    bool decoderAvailable = true;

    // The GPU was asleep (PCI runtime-suspended) this sample, so the probe left it alone rather than
    // wake it with sensor queries (#1117): every *Available flag above is then false. Linux reads
    // this from /sys/bus/pci/devices/<address>/power/runtime_status. memoryTotalBytes may still hold
    // the last total read while awake, so the adapter's VRAM size doesn't vanish while it sleeps.
    // Windows reads the device power state the PnP manager records (#1265); there PDH's utilization
    // and memory in use, the OS's own figures that never touch the GPU, may still be available.
    bool suspended = false;

    // Utilization (instantaneous snapshot, 0-100, provided by hardware/driver)
    double utilizationPercent = 0.0; // GPU usage reported by hardware
    // Note: memoryUtilPercent computed by Domain layer from memoryUsedBytes/memoryTotalBytes

    // Memory (bytes - raw counters)
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t memoryTotalBytes = 0;

    // Temperature (°C)
    std::int32_t temperatureC = 0;

    // Power (watts)
    double powerDrawWatts = 0.0;
    double powerLimitWatts = 0.0;

    // Cumulative energy (µJ), for a GPU whose driver reports an energy counter rather than power
    // (Intel i915/xe hwmon energy1_input, #1269). When energyAvailable, Domain derives powerDrawWatts
    // from the counter's change since the previous sample;
    // a sample without a readable previous counter (the first, or after a failed read, a suspend or
    // a counter reset) has no power.
    bool energyAvailable = false;
    std::uint64_t energyMicroJoules = 0;

    // Per-client cumulative engine busyness, for a GPU whose driver reports no utilization of its own
    // (Intel i915/xe, from each DRM client's fdinfo, #1267). When engineBusyAvailable, Domain derives
    // utilizationPercent from the clients in both this and the previous sample: per engine class, the
    // sum of their busy shares over the class's capacity, the busiest class being the GPU's
    // utilization; no clients means idle. A probe that couldn't see every client (an unreadable /proc,
    // a walk cut short, every fd directory denied) leaves engineBusyAvailable false. Without a previous sample, utilization is unread.
    bool engineBusyAvailable = false;
    std::vector<GPUEngineClientCounters> engineClients;

    // Clock speeds (MHz)
    std::uint32_t gpuClockMHz = 0;

    // Fan speed, raw (0 if not available) plus the device-reported max needed to normalize it.
    // Vendors report fan speed in different native units (NVML: already 0-100%; ROCm: a value
    // relative to RSMI_MAX_FAN_SPEED, not RPM despite older code here having assumed so -- see
    // #734), so Platform stores both raw numbers unconverted and Domain computes the
    // percentage (GPUSnapshot::fanSpeedPercent), consistent with how memoryUsedPercent is
    // derived from a raw counter pair. NVML probes set fanSpeedMaxRaw to
    // 100 since their raw reading already is a percentage.
    std::uint32_t fanSpeedRaw = 0;
    std::uint32_t fanSpeedMaxRaw = 0;

    // Video encoder/decoder utilization (0-100), as the driver reports it: NVML averages it over its
    // own sampling period (#1477). Read only where GPUCapabilities::hasEncoderDecoder says so.
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;
};

// Per-process GPU usage
struct ProcessGPUCounters
{
    std::int32_t pid = 0;
    std::string gpuId; // Which GPU

    // Memory allocated by the process on this GPU (bytes), kept apart as the adapter's own figures
    // are (#1164): dedicated is the GPU's own memory (VRAM; what NVML and ROCm SMI report per
    // process), shared is system memory the GPU maps for it (Windows' shared segment). Domain
    // compares each with the adapter's matching figure, so neither is summed into the other here.
    std::uint64_t gpuMemoryBytes = 0;       // dedicated
    std::uint64_t gpuSharedMemoryBytes = 0; // shared (0 where the platform has no such segment)

    // Utilization attributed to this process (0-100, instantaneous)
    double gpuUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;

    // Active engines (bitmask or string set)
    // Engines: 3D, Compute, Video Encode, Video Decode, Copy
    std::vector<std::string> activeEngines;
};

} // namespace Platform
