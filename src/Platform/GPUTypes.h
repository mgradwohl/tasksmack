#pragma once

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
    bool hasHotspotTemp = false;
    bool hasPowerMetrics = false;
    bool hasClockSpeeds = false;
    bool hasFanSpeed = false;
    bool hasPCIeMetrics = false;
    bool hasEngineUtilization = false;
    bool hasPerProcessMetrics = false; // Per-process GPU usage
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
/// (#1091). DXGI reports no PCI domain, so the domain is not part of the match.
struct PciLocation
{
    std::uint32_t bus = 0;
    std::uint32_t device = 0;
    bool operator==(const PciLocation&) const = default;
};

// Identifies a physical GPU
struct GPUInfo
{
    std::string id;     // Unique identifier (e.g., "GPU0", "GPU1")
    std::string luidId; // LUID-based identifier for PDH matching (e.g., "GPU_0x00000000_0x0000F78E")
    std::string name;   // Human-readable name (e.g., "NVIDIA GeForce RTX 2080 Ti")
    std::string vendor; // "NVIDIA", "AMD", "Intel", "Unknown"
    std::string driverVersion;
    bool isIntegrated = false;     // Integrated vs discrete
    std::uint32_t deviceIndex = 0; // Vendor-specific index
    /// The sensor metrics this particular adapter reports (temperature, hotspot, power, clocks,
    /// fan, PCIe, encoder/decoder); the other fields are not used. GPUCapabilities from a probe
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

    // The GPU was asleep (PCI runtime-suspended) this sample, so the probe left it alone rather than
    // wake it with sensor queries (#1117): every *Available flag above is then false. Linux reads
    // this from /sys/bus/pci/devices/<address>/power/runtime_status. memoryTotalBytes may still hold
    // the last total read while awake, so the adapter's VRAM size doesn't vanish while it sleeps.
    bool suspended = false;

    // Utilization (instantaneous snapshot, 0-100, provided by hardware/driver)
    double utilizationPercent = 0.0; // GPU usage reported by hardware
    // Note: memoryUtilPercent computed by Domain layer from memoryUsedBytes/memoryTotalBytes

    // Memory (bytes - raw counters)
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t memoryTotalBytes = 0;

    // Temperature (°C)
    std::int32_t temperatureC = 0;
    std::int32_t hotspotTempC = -1; // -1 if not available

    // Power (watts)
    double powerDrawWatts = 0.0;
    double powerLimitWatts = 0.0;

    // Cumulative energy (µJ), for a GPU whose driver reports an energy counter rather than power
    // (Intel i915/xe hwmon energy1_input, #1269). When energyAvailable, Domain derives powerDrawWatts
    // from the counter's change since the previous sample, as it does for the PCIe byte counters;
    // a sample without a readable previous counter (the first, or after a failed read, a suspend or
    // a counter reset) has no power.
    bool energyAvailable = false;
    std::uint64_t energyMicroJoules = 0;

    // Clock speeds (MHz)
    std::uint32_t gpuClockMHz = 0;
    std::uint32_t memoryClockMHz = 0;

    // Fan speed, raw (0 if not available) plus the device-reported max needed to normalize it.
    // Vendors report fan speed in different native units (NVML: already 0-100%; ROCm: a value
    // relative to RSMI_MAX_FAN_SPEED, not RPM despite older code here having assumed so -- see
    // #734), so Platform stores both raw numbers unconverted and Domain computes the
    // percentage (GPUSnapshot::fanSpeedPercent), consistent with how memoryUsedPercent and
    // powerUtilPercent are derived from raw counter pairs. NVML probes set fanSpeedMaxRaw to
    // 100 since their raw reading already is a percentage.
    std::uint32_t fanSpeedRaw = 0;
    std::uint32_t fanSpeedMaxRaw = 0;

    // PCIe throughput (cumulative bytes)
    std::uint64_t pcieTxBytes = 0;
    std::uint64_t pcieRxBytes = 0;

    // Engine utilization (0-100, instantaneous)
    double computeUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;
};

// Per-process GPU usage
struct ProcessGPUCounters
{
    std::int32_t pid = 0;
    std::string gpuId; // Which GPU

    // Memory allocated by process (bytes)
    std::uint64_t gpuMemoryBytes = 0;

    // Utilization attributed to this process (0-100, instantaneous)
    double gpuUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;

    // Active engines (bitmask or string set)
    // Engines: 3D, Compute, Video Encode, Video Decode, Copy
    std::vector<std::string> activeEngines;
};

} // namespace Platform
