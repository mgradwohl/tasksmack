#pragma once

#include "Platform/GPUTypes.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Platform
{

/// Normalize a GPU name for comparison: lowercase, and collapse runs of whitespace
/// (including leading/trailing) to single spaces / nothing. Used to match DXGI and NVML
/// names for the same physical adapter, which are rarely byte-identical.
[[nodiscard]] inline std::string normalizeGPUName(const std::string& name)
{
    std::string normalized;
    normalized.reserve(name.size());

    // Convert to lowercase and remove extra whitespace
    bool lastWasSpace = true; // Skip leading spaces
    for (const char c : name)
    {
        if (std::isspace(static_cast<unsigned char>(c)) != 0)
        {
            if (!lastWasSpace)
            {
                normalized += ' ';
                lastWasSpace = true;
            }
        }
        else
        {
            normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            lastWasSpace = false;
        }
    }

    // Remove trailing space
    if (!normalized.empty() && normalized.back() == ' ')
    {
        normalized.pop_back();
    }

    return normalized;
}

/// Check if two GPU names refer to the same adapter, handling DXGI vs NVML name
/// differences (case, whitespace, and one being a substring of the other, e.g.
/// "NVIDIA GeForce RTX 4090" vs "GeForce RTX 4090").
[[nodiscard]] inline bool gpuNamesMatch(const std::string& name1, const std::string& name2)
{
    // Normalize first (rather than trying a raw exact-match shortcut before this): two
    // raw-identical strings always normalize equal too, so nothing is lost, and computing the
    // normalized emptiness check up front - before any equality check - closes the case a raw
    // exact-match shortcut would otherwise miss, such as two distinct all-whitespace names
    // ("   " == "   ") that are byte-identical but must not count as a match.
    const std::string norm1 = normalizeGPUName(name1);
    const std::string norm2 = normalizeGPUName(name2);

    // An empty (or all-whitespace) name - e.g. NVML's DeviceGetName failed for this device -
    // must never match anything, including another empty/whitespace-only name: "" == "" and
    // "".contains("") are both true, which would otherwise let a nameless device spuriously
    // match every other GPU below.
    if (norm1.empty() || norm2.empty())
    {
        return false;
    }

    if (norm1 == norm2)
    {
        return true;
    }

    // Check if one contains the other (handles "NVIDIA GeForce..." vs "GeForce...")
    if (norm1.contains(norm2) || norm2.contains(norm1))
    {
        return true;
    }

    return false;
}

/// Pure merge logic extracted from WindowsGPUProbe::mergeNVMLEnhancements() so it can be unit
/// tested with fabricated counter vectors and mappings - the real function requires NVML
/// hardware and a live DXGI<->NVML index mapping, neither of which every dev/CI machine has.
/// Overwrites the enhanced fields (temperature, power, clocks, fan, utilization, and memory
/// when NVML reports a total) directly on @p dxgiCounters for every DXGI index present in
/// @p dxgiToNvmlMap, and returns the gpuId of each counter that was updated (so a later merge
/// step - e.g. PDH per-adapter utilization - knows not to overwrite it).
[[nodiscard]] inline std::unordered_set<std::string>
mergeNVMLIntoDXGICounters(std::vector<GPUCounters>& dxgiCounters,
                          const std::vector<GPUCounters>& nvmlCounters,
                          const std::unordered_map<std::uint32_t, std::uint32_t>& dxgiToNvmlMap)
{
    std::unordered_set<std::string> nvmlSourcedIds;
    if (nvmlCounters.empty())
    {
        return nvmlSourcedIds;
    }

    for (std::size_t dxgiIdx = 0; dxgiIdx < dxgiCounters.size(); ++dxgiIdx)
    {
        const auto mapIt = dxgiToNvmlMap.find(static_cast<std::uint32_t>(dxgiIdx));
        if (mapIt == dxgiToNvmlMap.end())
        {
            continue; // No NVML mapping for this GPU (not NVIDIA or not matched)
        }

        const std::uint32_t nvmlIdx = mapIt->second;
        if (nvmlIdx >= nvmlCounters.size())
        {
            continue; // Invalid mapping
        }

        auto& dxgiCounter = dxgiCounters[dxgiIdx];
        const auto& nvmlCounter = nvmlCounters[nvmlIdx];

        // Enhance with NVML data (NVML provides more accurate/detailed metrics)
        dxgiCounter.temperatureC = nvmlCounter.temperatureC;
        dxgiCounter.powerDrawWatts = nvmlCounter.powerDrawWatts;
        dxgiCounter.powerLimitWatts = nvmlCounter.powerLimitWatts;
        dxgiCounter.gpuClockMHz = nvmlCounter.gpuClockMHz;
        dxgiCounter.memoryClockMHz = nvmlCounter.memoryClockMHz;
        dxgiCounter.fanSpeedRaw = nvmlCounter.fanSpeedRaw;
        dxgiCounter.fanSpeedMaxRaw = nvmlCounter.fanSpeedMaxRaw;

        // Use NVML GPU utilization (NVML provides the actual GPU utilization, DXGI doesn't).
        // Always use NVML utilization when available, even if it's 0 (which is valid at idle).
        dxgiCounter.utilizationPercent = nvmlCounter.utilizationPercent;
        nvmlSourcedIds.insert(dxgiCounter.gpuId); // Track so PDH merge doesn't overwrite a valid 0%

        // Prefer NVML memory metrics (more accurate)
        if (nvmlCounter.memoryTotalBytes > 0)
        {
            dxgiCounter.memoryUsedBytes = nvmlCounter.memoryUsedBytes;
            dxgiCounter.memoryTotalBytes = nvmlCounter.memoryTotalBytes;
        }
    }
    return nvmlSourcedIds;
}

/// True when every counter in @p dxgiCounters already has NVML-sourced utilization, meaning
/// the PDH per-adapter utilization merge in WindowsGPUProbe::mergePDHAdapterUtilization() can be
/// skipped entirely. An empty @p dxgiCounters is never "all covered" (there's nothing to skip
/// usefully skipping for), matching the original inline check.
[[nodiscard]] inline bool allGPUsHaveNVMLUtilization(const std::vector<GPUCounters>& dxgiCounters,
                                                     const std::unordered_set<std::string>& nvmlSourcedIds)
{
    return !dxgiCounters.empty() &&
           std::ranges::all_of(dxgiCounters, [&nvmlSourcedIds](const auto& counter) { return nvmlSourcedIds.contains(counter.gpuId); });
}

/// Sums PDH per-process GPU utilization by GPU id (LUID-based, "GPU_0x...") so
/// mergePDHAdapterUtilization() can assign a single per-adapter total instead of the raw
/// per-process readings. Processes reporting 0% are skipped so an all-idle process list
/// legitimately produces an empty map (as opposed to a map full of zero entries).
[[nodiscard]] inline std::unordered_map<std::string, double>
sumProcessUtilizationByGPUId(const std::vector<ProcessGPUCounters>& processCounters)
{
    std::unordered_map<std::string, double> utilizationByGpuId;
    for (const auto& procCounter : processCounters)
    {
        if (procCounter.gpuUtilPercent > 0.0)
        {
            utilizationByGpuId[procCounter.gpuId] += procCounter.gpuUtilPercent;
        }
    }
    return utilizationByGpuId;
}

/// Pure assignment logic extracted from WindowsGPUProbe::mergePDHAdapterUtilization(): for each
/// DXGI counter not already covered by NVML, looks up its LUID-based id in @p dxgiIdToLuidId and,
/// if PDH reported a summed utilization for that LUID in @p utilizationByGpuId, assigns it
/// (clamped to [0, 100] since summing per-engine utilizations can exceed 100). Counters with no
/// LUID mapping or no matching PDH data are left untouched (utilization stays whatever the
/// caller initialized it to, typically 0).
inline void assignPDHUtilizationToDXGICounters(std::vector<GPUCounters>& dxgiCounters,
                                               const std::unordered_map<std::string, double>& utilizationByGpuId,
                                               const std::unordered_map<std::string, std::string>& dxgiIdToLuidId,
                                               const std::unordered_set<std::string>& nvmlSourcedIds)
{
    for (auto& dxgiCounter : dxgiCounters)
    {
        if (nvmlSourcedIds.contains(dxgiCounter.gpuId))
        {
            continue; // Already filled by NVML
        }

        const auto mapIt = dxgiIdToLuidId.find(dxgiCounter.gpuId);
        if (mapIt == dxgiIdToLuidId.end())
        {
            continue; // No LUID mapping available (enumerateGPUs not yet called)
        }

        const auto utilIt = utilizationByGpuId.find(mapIt->second);
        if (utilIt != utilizationByGpuId.end())
        {
            dxgiCounter.utilizationPercent = std::clamp(utilIt->second, 0.0, 100.0);
        }
        // If no PDH data found for this GPU's LUID, utilization stays untouched
    }
}

} // namespace Platform
