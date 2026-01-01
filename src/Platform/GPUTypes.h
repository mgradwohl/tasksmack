#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

// Identifies a physical GPU
struct GPUInfo
{
    std::string id;     // Unique identifier (PCI address, UUID)
    std::string name;   // Human-readable name
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

    // Fan speed (RPM, 0 if not available)
    std::uint32_t fanSpeedRPM = 0;

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
