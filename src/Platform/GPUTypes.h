#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

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
};

// Raw GPU counters (Platform layer provides raw values only)
// Derived metrics (rates, percentages) are computed by Domain layer
struct GPUCounters
{
    std::string gpuId; // Associates with GPUInfo

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

} // namespace Platform
