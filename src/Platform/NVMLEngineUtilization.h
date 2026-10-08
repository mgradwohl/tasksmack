#pragma once

// Platform-neutral reading of NVML's video-engine utilization
// (nvmlDeviceGetEncoderUtilization / nvmlDeviceGetDecoderUtilization), shared by the Linux and
// Windows NVML probes (#1477, #1485), as NVMLRunningProcesses.h is (#1313). Pure logic over an
// injected (possibly unresolved) query, so it is unit-tested without a driver.

#include "Platform/NVMLTypes.h"

namespace Platform::NVMLEngineUtilization
{

/// nvmlDeviceGetEncoderUtilization / nvmlDeviceGetDecoderUtilization: the video engine's
/// utilization (0-100) and the period it was averaged over, in microseconds (#1477).
using EngineUtilizationFn = NVML::nvmlReturn_t (*)(NVML::nvmlDevice_t, unsigned int*, unsigned int*);

/// One read of a video engine's utilization. `result` is NVML_ERROR_FUNCTION_NOT_FOUND when the
/// library doesn't export the query; `percent` is meaningful only when `result` is NVML_SUCCESS.
struct EngineUtilizationReading
{
    NVML::nvmlReturn_t result = NVML::NVML_ERROR_FUNCTION_NOT_FOUND;
    unsigned int percent = 0;
};

/// Reads a video engine's utilization through the (possibly unresolved) query. NVML averages it
/// over its own sampling period, which it reports alongside; only the percentage is kept, as for
/// nvmlDeviceGetUtilizationRates(), whose period NVML doesn't report at all.
[[nodiscard]] inline EngineUtilizationReading readEngineUtilization(EngineUtilizationFn query, NVML::nvmlDevice_t device)
{
    if (query == nullptr)
    {
        return {};
    }
    unsigned int percent = 0;
    unsigned int samplingPeriodUs = 0;
    const auto result = query(device, &percent, &samplingPeriodUs);
    return {.result = result, .percent = result == NVML::NVML_SUCCESS ? percent : 0U};
}

/// Whether a read says the device has the engine. Like the other per-device sensors, only "not
/// supported" (or the query missing from the library) means it lacks one: a transient failure
/// (a timeout, a busy GPU) must not hide the series for the session (#1111, #1112).
[[nodiscard]] constexpr bool engineUtilizationSupported(NVML::nvmlReturn_t result) noexcept
{
    return result != NVML::NVML_ERROR_NOT_SUPPORTED && result != NVML::NVML_ERROR_FUNCTION_NOT_FOUND;
}

} // namespace Platform::NVMLEngineUtilization
