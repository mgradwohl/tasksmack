#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Domain
{

// Immutable snapshot of a single GPU at a point in time
struct GPUSnapshot
{
    // Identity
    std::string gpuId;  // Unique identifier (e.g., "GPU0", "GPU1")
    std::string luidId; // LUID-based identifier for PDH matching (e.g., "GPU_0x00000000_0x0000F78E")
    std::string name;
    std::string vendor;
    bool isIntegrated = false;

    // Utilization (0-100)
    double utilizationPercent = 0.0;
    double memoryUtilPercent = 0.0;

    // Memory
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t memoryTotalBytes = 0;
    double memoryUsedPercent = 0.0; // Computed by Domain

    // Temperature
    std::int32_t temperatureC = 0;
    std::int32_t hotspotTempC = -1;

    // Power
    double powerDrawWatts = 0.0;
    double powerLimitWatts = 0.0;
    double powerUtilPercent = 0.0; // Computed by Domain

    // Clock speeds
    std::uint32_t gpuClockMHz = 0;
    std::uint32_t memoryClockMHz = 0;

    // Fan
    std::uint32_t fanSpeedRPMPercent = 0;

    // PCIe bandwidth (rates computed from deltas)
    double pcieTxBytesPerSec = 0.0;
    double pcieRxBytesPerSec = 0.0;

    // Engine utilization
    double computeUtilPercent = 0.0;
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;
};

} // namespace Domain
