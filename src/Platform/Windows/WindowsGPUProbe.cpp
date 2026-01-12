#include "WindowsGPUProbe.h"

#include "D3DKMTGPUProbe.h"
#include "DXGIGPUProbe.h"
#include "NVMLGPUProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <ranges>

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
    bool lastWasSpace = true;  // Skip leading spaces
    for (char c : name)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
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

}  // namespace

WindowsGPUProbe::WindowsGPUProbe()
    : m_DXGIProbe(std::make_unique<DXGIGPUProbe>()),
      m_NVMLProbe(std::make_unique<NVMLGPUProbe>()),
      m_D3DKMTProbe(std::make_unique<D3DKMTGPUProbe>())
{
    std::string probeSummary = "DXGI";
    if (m_NVMLProbe->isAvailable())
    {
        probeSummary += " + NVML";
    }
    if (m_D3DKMTProbe->capabilities().hasPerProcessMetrics)
    {
        probeSummary += " + D3DKMT";
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
                                     dxgiIdx, nvmlIdx, dxgiGPU.name, nvmlGPU.name);
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
        spdlog::debug("WindowsGPUProbe::mergeNVMLEnhancements: GPU {} utilization = {}%",
                      dxgiIdx,
                      nvmlCounter.utilizationPercent);

        // Prefer NVML memory metrics (more accurate)
        if (nvmlCounter.memoryTotalBytes > 0)
        {
            dxgiCounter.memoryUsedBytes = nvmlCounter.memoryUsedBytes;
            dxgiCounter.memoryTotalBytes = nvmlCounter.memoryTotalBytes;
        }
    }
}

std::vector<ProcessGPUCounters> WindowsGPUProbe::readProcessGPUCounters()
{
    std::vector<ProcessGPUCounters> allCounters;

    // Get per-process data from NVML (NVIDIA-specific, high-quality data)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCounters = m_NVMLProbe->readProcessGPUCounters();
        if (!nvmlCounters.empty())
        {
            spdlog::debug("WindowsGPUProbe: Got {} per-process GPU counters from NVML", nvmlCounters.size());
            allCounters = std::move(nvmlCounters);
        }
    }

    // Fall back to D3DKMT if NVML didn't provide data (works for all vendors)
    if (allCounters.empty() && m_D3DKMTProbe)
    {
        auto d3dkmtCounters = m_D3DKMTProbe->readProcessGPUCounters();
        if (!d3dkmtCounters.empty())
        {
            spdlog::debug("WindowsGPUProbe: Got {} per-process GPU counters from D3DKMT", d3dkmtCounters.size());
            allCounters = std::move(d3dkmtCounters);
        }
    }

    return allCounters;
}

GPUCapabilities WindowsGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Start with DXGI capabilities
    if (m_DXGIProbe)
    {
        caps = m_DXGIProbe->capabilities();
    }

    // Merge NVML capabilities (OR operation - if either supports, we support)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCaps = m_NVMLProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || nvmlCaps.hasTemperature;
        caps.hasHotspotTemp = caps.hasHotspotTemp || nvmlCaps.hasHotspotTemp;
        caps.hasPowerMetrics = caps.hasPowerMetrics || nvmlCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || nvmlCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || nvmlCaps.hasFanSpeed;
        caps.hasPCIeMetrics = caps.hasPCIeMetrics || nvmlCaps.hasPCIeMetrics;
        caps.hasEngineUtilization = caps.hasEngineUtilization || nvmlCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || nvmlCaps.hasPerProcessMetrics;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || nvmlCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || nvmlCaps.supportsMultiGPU;
    }

    // Merge D3DKMT capabilities (per-process metrics for all vendors)
    if (m_D3DKMTProbe)
    {
        auto d3dkmtCaps = m_D3DKMTProbe->capabilities();

        caps.hasEngineUtilization = caps.hasEngineUtilization || d3dkmtCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || d3dkmtCaps.hasPerProcessMetrics;
        caps.supportsMultiGPU = caps.supportsMultiGPU || d3dkmtCaps.supportsMultiGPU;
    }

    return caps;
}

} // namespace Platform
