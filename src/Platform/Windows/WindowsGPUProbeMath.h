#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/Windows/PDHGPUProbe.h"

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

/// Map each NVIDIA DXGI adapter (by index) to the NVML device (by index) with a matching name.
///
/// Each NVML device is claimed at most once, in enumeration order: two identical cards both used
/// to map to NVML device 0, so the second card's charts showed the first card's data (#1040).
/// DXGI exposes no PCI location to match on exactly, so with identical names the order is the
/// tie-break; both APIs enumerate by PCI order in practice.
[[nodiscard]] inline std::unordered_map<std::uint32_t, std::uint32_t> mapDXGIToNVML(const std::vector<GPUInfo>& dxgiGPUs,
                                                                                    const std::vector<GPUInfo>& nvmlGPUs)
{
    std::unordered_map<std::uint32_t, std::uint32_t> mapping;
    std::vector<bool> claimed(nvmlGPUs.size(), false);
    for (std::size_t dxgiIdx = 0; dxgiIdx < dxgiGPUs.size(); ++dxgiIdx)
    {
        if (dxgiGPUs[dxgiIdx].vendor != "NVIDIA")
        {
            continue;
        }
        for (std::size_t nvmlIdx = 0; nvmlIdx < nvmlGPUs.size(); ++nvmlIdx)
        {
            if (!claimed[nvmlIdx] && gpuNamesMatch(dxgiGPUs[dxgiIdx].name, nvmlGPUs[nvmlIdx].name))
            {
                mapping[static_cast<std::uint32_t>(dxgiIdx)] = static_cast<std::uint32_t>(nvmlIdx);
                claimed[nvmlIdx] = true;
                break;
            }
        }
    }
    return mapping;
}

/// Set each DXGI adapter's sensorCapabilities: sensor metrics come only from NVML on Windows, so a
/// mapped adapter takes its own NVML device's set (or @p nvmlProbeCaps if the device has none), and
/// an adapter NVML does not cover -- e.g. a hybrid laptop's Intel iGPU -- has no sensors (#1040).
inline void assignSensorCapabilities(std::vector<GPUInfo>& dxgiGPUs,
                                     const std::vector<GPUInfo>& nvmlGPUs,
                                     const std::unordered_map<std::uint32_t, std::uint32_t>& dxgiToNVML,
                                     const GPUCapabilities& nvmlProbeCaps)
{
    for (std::size_t dxgiIdx = 0; dxgiIdx < dxgiGPUs.size(); ++dxgiIdx)
    {
        const auto mapped = dxgiToNVML.find(static_cast<std::uint32_t>(dxgiIdx));
        if (mapped == dxgiToNVML.end() || mapped->second >= nvmlGPUs.size())
        {
            dxgiGPUs[dxgiIdx].sensorCapabilities = GPUCapabilities{};
            continue;
        }
        dxgiGPUs[dxgiIdx].sensorCapabilities = nvmlGPUs[mapped->second].sensorCapabilities.value_or(nvmlProbeCaps);
    }
}

/// Fill memoryUsedBytes for every adapter whose memory NVML did not supply, from PDH's adapter-wide
/// counters. @p nvmlMemoryIds are the GPUs whose NVML memory read succeeded (see
/// mergeNVMLIntoDXGICounters()), not merely the ones NVML covers for utilization.
///
/// DXGI's QueryVideoMemoryInfo reports the *calling process's* usage, so the GPU tab used to show
/// TaskSmack's own few MB as the GPU's memory (#1029). An integrated GPU's memory is the shared
/// segment; a discrete GPU's is its dedicated VRAM.
inline void assignPDHMemoryToDXGICounters(std::vector<GPUCounters>& dxgiCounters,
                                          const std::unordered_map<std::string, AdapterMemoryUsage>& memoryByLuidId,
                                          const std::unordered_map<std::string, std::string>& dxgiIdToLuidId,
                                          const std::unordered_map<std::string, bool>& dxgiIdIsIntegrated,
                                          const std::unordered_set<std::string>& nvmlMemoryIds)
{
    for (auto& counter : dxgiCounters)
    {
        if (nvmlMemoryIds.contains(counter.gpuId))
        {
            continue;
        }
        const auto luidIt = dxgiIdToLuidId.find(counter.gpuId);
        if (luidIt == dxgiIdToLuidId.end())
        {
            continue;
        }
        const auto memIt = memoryByLuidId.find(luidIt->second);
        if (memIt == memoryByLuidId.end())
        {
            continue;
        }
        const auto integratedIt = dxgiIdIsIntegrated.find(counter.gpuId);
        const bool integrated = (integratedIt != dxgiIdIsIntegrated.end()) && integratedIt->second;
        counter.memoryUsedBytes = integrated ? memIt->second.sharedBytes : memIt->second.dedicatedBytes;
    }
}

/// Pure merge logic extracted from WindowsGPUProbe::mergeNVMLEnhancements() so it can be unit
/// tested with fabricated counter vectors and mappings - the real function requires NVML
/// hardware and a live DXGI<->NVML index mapping, neither of which every dev/CI machine has.
/// Overwrites the enhanced fields (temperature, power, clocks, fan, utilization, and memory
/// when NVML reports a total) directly on @p dxgiCounters for every DXGI index present in
/// @p dxgiToNvmlMap, and returns the gpuId of each counter that was updated (so a later merge
/// step - e.g. PDH per-adapter utilization - knows not to overwrite it).
///
/// @param nvmlMemoryIds  If given, receives the gpuId of each counter whose memory actually came
///                       from NVML (a non-zero total). A GPU NVML covers for utilization but whose
///                       memory read failed still needs the PDH memory fallback (#1029).
[[nodiscard]] inline std::unordered_set<std::string>
mergeNVMLIntoDXGICounters(std::vector<GPUCounters>& dxgiCounters,
                          const std::vector<GPUCounters>& nvmlCounters,
                          const std::unordered_map<std::uint32_t, std::uint32_t>& dxgiToNvmlMap,
                          std::unordered_set<std::string>* nvmlMemoryIds = nullptr)
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
        if (nvmlCounter.gpuId.empty())
        {
            continue; // Placeholder from orderNVMLCountersByIds(): this device was not read
        }

        // Enhance with NVML data (NVML provides more accurate/detailed metrics). Whether each read
        // succeeded comes along, so an unread field publishes as a gap, not a real-looking 0 (#1111).
        dxgiCounter.temperatureC = nvmlCounter.temperatureC;
        dxgiCounter.temperatureAvailable = nvmlCounter.temperatureAvailable;
        dxgiCounter.powerDrawWatts = nvmlCounter.powerDrawWatts;
        dxgiCounter.powerAvailable = nvmlCounter.powerAvailable;
        dxgiCounter.powerLimitWatts = nvmlCounter.powerLimitWatts;
        dxgiCounter.gpuClockMHz = nvmlCounter.gpuClockMHz;
        dxgiCounter.gpuClockAvailable = nvmlCounter.gpuClockAvailable;
        dxgiCounter.memoryClockMHz = nvmlCounter.memoryClockMHz;
        dxgiCounter.fanSpeedRaw = nvmlCounter.fanSpeedRaw;
        dxgiCounter.fanSpeedMaxRaw = nvmlCounter.fanSpeedMaxRaw;

        // Use NVML GPU utilization (NVML provides the actual GPU utilization, DXGI doesn't) when this
        // sample's read succeeded, even if it's 0 (valid at idle). When it failed, leave the GPU
        // un-NVML-sourced and mark its utilization unread: PDH's utilization -- a real reading --
        // replaces it and restores availability, and with no PDH sample it publishes as a gap rather
        // than DXGI's placeholder 0 (#1111).
        if (nvmlCounter.utilizationAvailable)
        {
            dxgiCounter.utilizationPercent = nvmlCounter.utilizationPercent;
            nvmlSourcedIds.insert(dxgiCounter.gpuId); // Track so PDH merge doesn't overwrite a valid 0%
        }
        else
        {
            dxgiCounter.utilizationAvailable = false;
        }

        // Prefer NVML memory metrics (more accurate)
        if (nvmlCounter.memoryTotalBytes > 0)
        {
            dxgiCounter.memoryUsedBytes = nvmlCounter.memoryUsedBytes;
            dxgiCounter.memoryTotalBytes = nvmlCounter.memoryTotalBytes;
            if (nvmlMemoryIds != nullptr)
            {
                nvmlMemoryIds->insert(dxgiCounter.gpuId);
            }
        }
    }
    return nvmlSourcedIds;
}

/// Reorder NVML counters into the order NVML enumerated its devices, matching by gpuId (the
/// device UUID, or "NVML_GPU<n>"), so a DXGI-to-NVML mapping built from enumeration positions
/// indexes the right device's counters. NVMLGPUProbe reads counters by iterating an unordered map
/// of device handles, so two devices can come back in either order (#1040). A device with no
/// counters this read gets a placeholder with an empty gpuId, which mergeNVMLIntoDXGICounters()
/// skips rather than applying zeros.
[[nodiscard]] inline std::vector<GPUCounters> orderNVMLCountersByIds(const std::vector<GPUCounters>& counters,
                                                                     const std::vector<std::string>& enumeratedIds)
{
    std::vector<GPUCounters> ordered(enumeratedIds.size());
    for (const auto& counter : counters)
    {
        const auto it = std::ranges::find(enumeratedIds, counter.gpuId);
        if (it != enumeratedIds.end())
        {
            ordered[static_cast<std::size_t>(it - enumeratedIds.begin())] = counter;
        }
    }
    return ordered;
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

/// Pure assignment logic extracted from WindowsGPUProbe::mergePDHAdapterUtilization(): for each
/// DXGI counter not already covered by NVML, looks up its LUID-based id in @p dxgiIdToLuidId and,
/// if PDH reported a utilization for that LUID in @p utilizationByGpuId, assigns it (clamped to
/// [0, 100]). Counters with no
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
            dxgiCounter.utilizationAvailable = true; // A real reading, even after a failed NVML read (#1111)
        }
        // If no PDH data found for this GPU's LUID, utilization (and its availability) stays untouched
    }
}

} // namespace Platform
