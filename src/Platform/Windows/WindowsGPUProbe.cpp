#include "WindowsGPUProbe.h"

#include "DXGIGPUProbe.h"
#include "NVMLGPUProbe.h"
#include "PDHGPUProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace Platform
{

namespace
{

// Normalize GPU name for comparison (trim whitespace, lowercase, remove prefixes)
std::string normalizeGPUName(const std::string& name)
{
    std::string normalized;
    normalized.reserve(name.size());

    // Convert to lowercase and remove extra whitespace
    bool lastWasSpace = true; // Skip leading spaces
    for (char c : name)
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

// Check if two GPU names match (handles DXGI vs NVML name differences)
bool gpuNamesMatch(const std::string& name1, const std::string& name2)
{
    // First try exact match
    if (name1 == name2)
    {
        return true;
    }

    // Try normalized comparison
    const std::string norm1 = normalizeGPUName(name1);
    const std::string norm2 = normalizeGPUName(name2);

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

} // namespace

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

    // Merge NVML enhancements for NVIDIA GPUs
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        mergeNVMLEnhancements(counters);
    }

    // For GPUs without NVML, merge PDH system-wide utilization data
    mergePDHSystemWideUtilization(counters);

    return counters;
}

void WindowsGPUProbe::mergeNVMLEnhancements(std::vector<GPUCounters>& dxgiCounters)
{
    if (!m_NVMLProbe || !m_NVMLProbe->isAvailable())
    {
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: NVML not available, skipping merge");
        return;
    }

    // Get NVML counters
    auto nvmlCounters = m_NVMLProbe->readGPUCounters();
    if (nvmlCounters.empty())
    {
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: NVML returned no counters");
        return;
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
        dxgiCounter.fanSpeedRPMPercent = nvmlCounter.fanSpeedRPMPercent;

        // Use NVML GPU utilization (NVML provides the actual GPU utilization, DXGI doesn't)
        // Always use NVML utilization when available, even if it's 0 (which is valid at idle)
        dxgiCounter.utilizationPercent = nvmlCounter.utilizationPercent;
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: GPU {} utilization = {}%", dxgiIdx, nvmlCounter.utilizationPercent);

        // Prefer NVML memory metrics (more accurate)
        if (nvmlCounter.memoryTotalBytes > 0)
        {
            dxgiCounter.memoryUsedBytes = nvmlCounter.memoryUsedBytes;
            dxgiCounter.memoryTotalBytes = nvmlCounter.memoryTotalBytes;
        }
    }
}

void WindowsGPUProbe::mergePDHSystemWideUtilization(std::vector<GPUCounters>& dxgiCounters)
{
    // Skip if no PDH or if all GPUs already have utilization data (from NVML)
    if (!m_PDHProbe || !m_PDHProbe->isAvailable())
    {
        return;
    }

    // Check if we need PDH at all - if all GPUs have non-zero utilization, skip
    // (they likely came from NVML or another source)
    bool allHaveUtilization = true;
    for (const auto& counter : dxgiCounters)
    {
        if (counter.utilizationPercent == 0.0)
        {
            allHaveUtilization = false;
            break;
        }
    }
    if (!dxgiCounters.empty() && allHaveUtilization)
    {
        return; // All GPUs have utilization data already
    }

    // Read per-process GPU data from PDH
    auto processCounters = m_PDHProbe->readProcessGPUCounters();
    if (processCounters.empty())
    {
        return;
    }

    // Sum GPU utilization across all processes to get system-wide total
    // PDH returns per-process GPU utilization which is additive across engines
    // System-wide utilization = sum of all per-process utilizations (capped at 100%)
    double totalUtilization = 0.0;
    int processCount = 0;
    for (const auto& procCounter : processCounters)
    {
        if (procCounter.gpuUtilPercent > 0.0)
        {
            totalUtilization += procCounter.gpuUtilPercent;
            processCount += 1;
        }
    }

    if (processCount > 0)
    {
        // Clamp to 0-100 range (utilization is percentage of single GPU capacity)
        totalUtilization = std::min(100.0, totalUtilization);

        // Assign to all GPUs that don't have utilization data
        for (auto& dxgiCounter : dxgiCounters)
        {
            if (dxgiCounter.utilizationPercent == 0.0)
            {
                // NOTE: This approach has a limitation in multi-GPU scenarios: we sum per-process
                // utilizations across ALL GPUs and assign that total to EACH GPU without NVML data.
                // Ideally we'd sum per-GPU using the LUID info from PDH, but that requires more
                // complex matching logic. For single-GPU or NVML-only systems, this works correctly.
                dxgiCounter.utilizationPercent = totalUtilization;
                spdlog::debug("WindowsGPUProbe::mergePDHSystemWideUtilization: GPU {} utilization = {}% (summed from {} processes)",
                              dxgiCounter.gpuId,
                              totalUtilization,
                              processCount);
            }
        }
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

void WindowsGPUProbe::setInstanceRefreshInterval(std::chrono::seconds interval)
{
    if (m_PDHProbe)
    {
        m_PDHProbe->setInstanceRefreshInterval(interval);
    }
}

} // namespace Platform
