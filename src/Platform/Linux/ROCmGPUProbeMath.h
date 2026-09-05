#pragma once

// Pure, injectable-function-pointer logic extracted from ROCmGPUProbe.cpp so it can be
// unit-tested directly, without going through dlopen()/the ROCm SMI mock library. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include <cstdint>
#include <string>

namespace Platform::ROCmGPUProbeMath
{

// Mirrors ROCm SMI's RSMI_STATUS_SUCCESS (0) without depending on ROCmGPUProbe.cpp's
// anonymous-namespace rsmi_status_t/RSMI_STATUS_SUCCESS definitions.
inline constexpr std::uint32_t kRsmiStatusSuccess = 0;

using StatusStringFn = const char* (*) (std::uint32_t);
using DeviceIdLookupFn = std::uint32_t (*)(std::uint32_t, std::uint64_t*);

/// Resolves a ROCm SMI status code to a human-readable string via the (possibly unresolved)
/// rsmi_status_string function pointer, falling back to "Unknown ROCm error N" when the
/// symbol failed to load or the library itself returns null.
[[nodiscard]] inline std::string resolveErrorString(std::uint32_t result, StatusStringFn statusStringFn)
{
    if (statusStringFn != nullptr)
    {
        const char* errStr = statusStringFn(result);
        if (errStr != nullptr)
        {
            return errStr;
        }
    }
    return "Unknown ROCm error " + std::to_string(result);
}

/// Derives a device identifier via the fallback chain: uniqueId -> pciId -> "amd_<index>".
/// Must match enumerateGPUs()'s chain exactly so GPUCounters::gpuId correlates to
/// GPUInfo::id in the domain layer. Either lookup function pointer may be null (optional
/// symbol failed to load) or may fail at call time (non-success status).
[[nodiscard]] inline std::string deriveDeviceId(std::uint32_t deviceIdx, DeviceIdLookupFn uniqueIdFn, DeviceIdLookupFn pciIdFn)
{
    std::uint64_t uniqueId = 0;
    if (uniqueIdFn != nullptr && uniqueIdFn(deviceIdx, &uniqueId) == kRsmiStatusSuccess)
    {
        return std::to_string(uniqueId);
    }

    std::uint64_t pciId = 0;
    if (pciIdFn != nullptr && pciIdFn(deviceIdx, &pciId) == kRsmiStatusSuccess)
    {
        return std::to_string(pciId);
    }

    return "amd_" + std::to_string(deviceIdx);
}

} // namespace Platform::ROCmGPUProbeMath
