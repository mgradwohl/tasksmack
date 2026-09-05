#include "WindowsGPUProbe.h"

#include "DXGIGPUProbe.h"
#include "NVMLGPUProbe.h"
#include "PDHGPUProbe.h"
#include "Platform/GPUTypes.h"
#include "WindowsGPUProbeMath.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Platform
{

WindowsGPUProbe::WindowsGPUProbe()
    : m_DXGIProbe(std::make_unique<DXGIGPUProbe>()),
      m_NVMLProbe(std::make_unique<NVMLGPUProbe>()),
      m_PDHProbe(std::make_unique<PDHGPUProbe>())
{
    std::string probeSummary = "DXGI";
    if (m_NVMLProbe->isAvailable())
    {
        probeSummary += " + NVML";
    }
    if (m_PDHProbe->isAvailable())
    {
        probeSummary += " + PDH";
    }
    spdlog::debug("WindowsGPUProbe: Initialized with {} probe(s)", probeSummary);
}

WindowsGPUProbe::~WindowsGPUProbe() = default;

std::vector<GPUInfo> WindowsGPUProbe::enumerateGPUs()
{
    // Use DXGI as primary enumeration source (works for all vendors)
    if (m_DXGIProbe)
    {
        auto gpus = m_DXGIProbe->enumerateGPUs();

        // If NVML is available, try to match NVIDIA GPUs for enhanced data
        if (m_NVMLProbe && m_NVMLProbe->isAvailable())
        {
            auto nvmlGPUs = m_NVMLProbe->enumerateGPUs();
            spdlog::debug("WindowsGPUProbe: Found {} DXGI GPUs and {} NVML GPUs", gpus.size(), nvmlGPUs.size());

            // Build mapping between DXGI and NVML GPUs (match by name)
            m_DXGIToNVMLMap.clear();
            for (std::size_t dxgiIdx = 0; dxgiIdx < gpus.size(); ++dxgiIdx)
            {
                const auto& dxgiGPU = gpus[dxgiIdx];
                spdlog::debug("WindowsGPUProbe: DXGI GPU {}: '{}' (vendor: {})", dxgiIdx, dxgiGPU.name, dxgiGPU.vendor);

                // Only try to match NVIDIA GPUs
                if (dxgiGPU.vendor != "NVIDIA")
                {
                    continue;
                }

                // Find matching NVML GPU by name
                bool matched = false;
                for (std::size_t nvmlIdx = 0; nvmlIdx < nvmlGPUs.size(); ++nvmlIdx)
                {
                    const auto& nvmlGPU = nvmlGPUs[nvmlIdx];
                    spdlog::debug("WindowsGPUProbe: Comparing DXGI '{}' with NVML '{}'", dxgiGPU.name, nvmlGPU.name);

                    // Match by name using robust comparison
                    if (gpuNamesMatch(dxgiGPU.name, nvmlGPU.name))
                    {
                        m_DXGIToNVMLMap[static_cast<uint32_t>(dxgiIdx)] = static_cast<uint32_t>(nvmlIdx);
                        spdlog::info("WindowsGPUProbe: Mapped DXGI GPU {} to NVML GPU {} ('{}' <-> '{}')",
                                     dxgiIdx,
                                     nvmlIdx,
                                     dxgiGPU.name,
                                     nvmlGPU.name);
                        matched = true;
                        break;
                    }
                }

                if (!matched)
                {
                    spdlog::warn("WindowsGPUProbe: Failed to match DXGI GPU '{}' with any NVML GPU", dxgiGPU.name);
                }
            }

            spdlog::info("WindowsGPUProbe: Created {} DXGI-to-NVML mappings", m_DXGIToNVMLMap.size());
        }
        else
        {
            spdlog::debug("WindowsGPUProbe: NVML not available for NVIDIA GPU enhancement");
        }

        // Build DXGI id → LUID map for PDH per-GPU utilization matching.
        // PDH process counters use "GPU_0x{High}_0x{Low}" as their gpuId; DXGI
        // stores the same value in GPUInfo::luidId. We need to look up a counter's
        // LUID from its index-based gpuId ("GPU0", "GPU1", …) to match PDH data.
        // Clear before rebuilding because enumerateGPUs() may be called multiple times
        // (e.g., on device change) and the adapter list can change between calls.
        m_DXGIIdToLuidId.clear();
        for (const auto& gpu : gpus)
        {
            if (!gpu.luidId.empty())
            {
                m_DXGIIdToLuidId[gpu.id] = gpu.luidId;
                spdlog::debug("WindowsGPUProbe: LUID mapping: {} → {}", gpu.id, gpu.luidId);
            }
        }

        return gpus;
    }

    return {};
}

std::vector<GPUCounters> WindowsGPUProbe::readGPUCounters()
{
    if (!m_DXGIProbe)
    {
        return {};
    }

    // Get base counters from DXGI
    auto counters = m_DXGIProbe->readGPUCounters();

    // Merge NVML enhancements for NVIDIA GPUs; returns IDs that got NVML utilization
    std::unordered_set<std::string> nvmlSourcedIds;
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        nvmlSourcedIds = mergeNVMLEnhancements(counters);
    }

    // For GPUs without NVML, merge PDH per-adapter utilization matched to each adapter
    mergePDHAdapterUtilization(counters, nvmlSourcedIds);

    return counters;
}

std::unordered_set<std::string> WindowsGPUProbe::mergeNVMLEnhancements(std::vector<GPUCounters>& dxgiCounters)
{
    std::unordered_set<std::string> nvmlSourcedIds;
    if (!m_NVMLProbe || !m_NVMLProbe->isAvailable())
    {
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: NVML not available, skipping merge");
        return nvmlSourcedIds;
    }

    // Get NVML counters
    auto nvmlCounters = m_NVMLProbe->readGPUCounters();
    if (nvmlCounters.empty())
    {
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: NVML returned no counters");
        return nvmlSourcedIds;
    }

    spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: Have {} DXGI counters and {} NVML counters, {} mappings",
                  dxgiCounters.size(),
                  nvmlCounters.size(),
                  m_DXGIToNVMLMap.size());

    // Merge NVML data into DXGI counters based on mapping
    for (std::size_t dxgiIdx = 0; dxgiIdx < dxgiCounters.size(); ++dxgiIdx)
    {
        auto mapIt = m_DXGIToNVMLMap.find(static_cast<uint32_t>(dxgiIdx));
        if (mapIt == m_DXGIToNVMLMap.end())
        {
            spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: No NVML mapping for DXGI GPU {}", dxgiIdx);
            continue; // No NVML mapping for this GPU (not NVIDIA or not matched)
        }

        const uint32_t nvmlIdx = mapIt->second;
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

        // Use NVML GPU utilization (NVML provides the actual GPU utilization, DXGI doesn't)
        // Always use NVML utilization when available, even if it's 0 (which is valid at idle)
        dxgiCounter.utilizationPercent = nvmlCounter.utilizationPercent;
        nvmlSourcedIds.insert(dxgiCounter.gpuId); // Track so PDH merge doesn't overwrite a valid 0%
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: GPU {} utilization = {}%", dxgiIdx, nvmlCounter.utilizationPercent);

        // Prefer NVML memory metrics (more accurate)
        if (nvmlCounter.memoryTotalBytes > 0)
        {
            dxgiCounter.memoryUsedBytes = nvmlCounter.memoryUsedBytes;
            dxgiCounter.memoryTotalBytes = nvmlCounter.memoryTotalBytes;
        }
    }
    return nvmlSourcedIds;
}

void WindowsGPUProbe::mergePDHAdapterUtilization(std::vector<GPUCounters>& dxgiCounters,
                                                 const std::unordered_set<std::string>& nvmlSourcedIds)
{
    // Skip if no PDH or if all GPUs already have utilization data (from NVML)
    if (!m_PDHProbe || !m_PDHProbe->isAvailable())
    {
        return;
    }

    // Check if we need PDH at all - if all GPUs already have NVML utilization, skip
    // (0% at idle is a valid NVML reading, not a sentinel)
    bool const allHaveNvmlUtilization =
        !dxgiCounters.empty() &&
        std::ranges::all_of(dxgiCounters, [&nvmlSourcedIds](const auto& counter) { return nvmlSourcedIds.contains(counter.gpuId); });
    // Skip PDH if all GPUs already have NVML utilization data
    if (allHaveNvmlUtilization)
    {
        return;
    }

    // Read per-process GPU data from PDH
    auto processCounters = m_PDHProbe->readProcessGPUCounters();
    if (processCounters.empty())
    {
        return;
    }

    // Sum utilization per GPU LUID.
    // PDH ProcessGPUCounters::gpuId is "GPU_0x{HighPart}_0x{LowPart}" — the same
    // format as GPUInfo::luidId from DXGI. Group process contributions per GPU so
    // we assign the correct utilization to each physical adapter instead of the
    // system-wide sum to every adapter (which inflated multi-GPU readings).
    std::unordered_map<std::string, double> utilizationByLuid;
    for (const auto& procCounter : processCounters)
    {
        if (procCounter.gpuUtilPercent > 0.0)
        {
            utilizationByLuid[procCounter.gpuId] += procCounter.gpuUtilPercent;
        }
    }

    if (utilizationByLuid.empty())
    {
        return;
    }

    // Assign per-GPU utilization by matching each DXGI counter's LUID-based id
    // to the corresponding PDH bucket. m_DXGIIdToLuidId is populated in
    // enumerateGPUs() and maps "GPU0" → "GPU_0x00000000_0x0000D3A0".
    for (auto& dxgiCounter : dxgiCounters)
    {
        if (nvmlSourcedIds.contains(dxgiCounter.gpuId))
        {
            continue; // Already filled by NVML
        }

        auto mapIt = m_DXGIIdToLuidId.find(dxgiCounter.gpuId);
        if (mapIt == m_DXGIIdToLuidId.end())
        {
            // No LUID mapping available (enumerateGPUs not yet called).
            // Skip rather than falling back to the inflated system-wide total.
            spdlog::debug(
                "WindowsGPUProbe::mergePDHAdapterUtilization: No LUID mapping for GPU {}. Ensure enumerateGPUs() was called first.",
                dxgiCounter.gpuId);
            continue;
        }

        const auto& luidId = mapIt->second;
        auto utilIt = utilizationByLuid.find(luidId);
        if (utilIt != utilizationByLuid.end())
        {
            // Clamp to [0, 100] — summing engine utilizations can exceed 100,
            // so keep the final per-adapter value within the expected range.
            dxgiCounter.utilizationPercent = std::clamp(utilIt->second, 0.0, 100.0);
            spdlog::debug("WindowsGPUProbe::mergePDHAdapterUtilization: GPU {} ({}) utilization = {:.1f}%",
                          dxgiCounter.gpuId,
                          luidId,
                          dxgiCounter.utilizationPercent);
        }
        // If no PDH data found for this GPU's LUID, utilization stays 0
    }
}

std::vector<ProcessGPUCounters> WindowsGPUProbe::readProcessGPUCounters()
{
    std::vector<ProcessGPUCounters> allCounters;

    // Use PDH for per-process GPU data (utilization + memory)
    // This is the same mechanism Task Manager uses - works cross-vendor
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        auto pdhCounters = m_PDHProbe->readProcessGPUCounters();
        if (!pdhCounters.empty())
        {
            spdlog::debug("WindowsGPUProbe: Got {} per-process GPU entries from PDH (utilization + memory)", pdhCounters.size());
            allCounters = std::move(pdhCounters);
        }
    }

    // PDH provides both GPU Engine (utilization) and GPU Process Memory counters
    // No need to merge from other sources - PDH is the authoritative source for per-process data

    return allCounters;
}

GPUCapabilities WindowsGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Start with DXGI capabilities (basic enumeration)
    if (m_DXGIProbe)
    {
        caps = m_DXGIProbe->capabilities();
    }

    // Merge NVML capabilities for system-level GPU metrics (temp, power, clocks)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCaps = m_NVMLProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || nvmlCaps.hasTemperature;
        caps.hasHotspotTemp = caps.hasHotspotTemp || nvmlCaps.hasHotspotTemp;
        caps.hasPowerMetrics = caps.hasPowerMetrics || nvmlCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || nvmlCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || nvmlCaps.hasFanSpeed;
        caps.hasPCIeMetrics = caps.hasPCIeMetrics || nvmlCaps.hasPCIeMetrics;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || nvmlCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || nvmlCaps.supportsMultiGPU;
    }

    // Merge PDH capabilities for per-process metrics (works cross-vendor)
    if (m_PDHProbe && m_PDHProbe->isAvailable())
    {
        auto pdhCaps = m_PDHProbe->capabilities();

        caps.hasEngineUtilization = caps.hasEngineUtilization || pdhCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || pdhCaps.hasPerProcessMetrics;
        caps.supportsMultiGPU = caps.supportsMultiGPU || pdhCaps.supportsMultiGPU;
    }

    return caps;
}

} // namespace Platform
