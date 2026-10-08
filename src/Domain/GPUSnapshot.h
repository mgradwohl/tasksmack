#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Domain
{

// Snapshot of a single GPU at a point in time.
// Fields come from the platform probe and are treated as read-only once pushed
// into the history ring buffer.  captureTimeSec is the exception: it is 0 after
// computeSnapshot() and is stamped by GPUModel::refresh() just before the snapshot
// is pushed, so every entry carries its own monotonic timestamp.
struct GPUSnapshot
{
    // Sample time in seconds from steady_clock::time_since_epoch() (monotonic, not wall-clock).
    // Use only for relative age/duration calculations between samples, not as an absolute time.
    double captureTimeSec = 0.0;

    // False for a history placeholder recorded while this GPU was missing from a read (eGPU unplugged,
    // driver reset, failed enumeration). Its history values publish as NaN, so the charts show a gap
    // across the absence instead of a line joining the samples either side of it (#1146).
    bool sampled = true;

    // Identity
    std::string gpuId;  // Stable unique identifier: Platform::GPUInfo::id
    std::string luidId; // LUID-based identifier for PDH matching (e.g., "GPU_0x00000000_0x0000F78E")
    std::string name;
    std::string vendor;
    bool isIntegrated = false;
    // The memory used/total figures count the shared segment rather than dedicated VRAM
    // (Platform::GPUInfo::memoryIsShared): what a process's "GPU memory" on this GPU counts (#1164).
    bool memoryIsShared = false;

    // Whether this sample's read of each field succeeded. False when a supported sensor couldn't be
    // read this time (NVML_ERROR_TIMEOUT, GPU lost, a driver reset): its value is then meaningless,
    // not a real 0 -- GPUModel publishes NaN for it and the bar shows N/A (#1111). Default true, so a
    // probe that never fails a read needn't set them.
    bool utilizationAvailable = true;
    bool temperatureAvailable = true;
    bool powerAvailable = true;
    bool gpuClockAvailable = true;
    bool memoryAvailable = true; // used/total bytes, and so the memory percent
    // The GPU was asleep (runtime-suspended) and deliberately not queried this sample, so as not to
    // wake it (#1117). Its readings are all unavailable; the UI labels it as sleeping.
    bool suspended = false;

    // Utilization (0-100)
    double utilizationPercent = 0.0;
    double memoryUtilPercent = 0.0;

    // Memory
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t memoryTotalBytes = 0;
    double memoryUsedPercent = 0.0; // Computed by Domain

    // Temperature
    std::int32_t temperatureC = 0;

    // Power
    double powerDrawWatts = 0.0;
    double powerLimitWatts = 0.0;

    // Clock speeds
    std::uint32_t gpuClockMHz = 0;

    // Fan
    std::uint32_t fanSpeedPercent = 0; // Computed by Domain from fanSpeedRaw/fanSpeedMaxRaw
    // True only when this sample actually carried a fan reading (fanSpeedMaxRaw > 0). A probe
    // can advertise GPUCapabilities::hasFanSpeed (the sensor/symbol is available in general) yet
    // still fail to read it on a given poll (e.g. a transient RSMI call failure); without this
    // flag that looks identical to a genuine 0% reading. UI should gate display on this, not just
    // on the capability, to avoid showing a misleading "0%" for an unavailable sample.
    bool fanSpeedAvailable = false;

    // Engine utilization
    double encoderUtilPercent = 0.0;
    double decoderUtilPercent = 0.0;
};

} // namespace Domain
