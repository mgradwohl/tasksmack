#include "WindowsGPUProbe.h"

#include "D3DKMTGPUProbe.h"
#include "DXGIGPUProbe.h"
#include "NVMLGPUProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ranges>

namespace Platform
{

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

            // Build mapping between DXGI and NVML GPUs (match by name)
            m_DXGIToNVMLMap.clear();
            for (std::size_t dxgiIdx = 0; dxgiIdx < gpus.size(); ++dxgiIdx)
            {
                const auto& dxgiGPU = gpus[dxgiIdx];

                // Only try to match NVIDIA GPUs
                if (dxgiGPU.vendor != "NVIDIA")
                {
                    continue;
                }

                // Find matching NVML GPU by name
                for (std::size_t nvmlIdx = 0; nvmlIdx < nvmlGPUs.size(); ++nvmlIdx)
                {
                    const auto& nvmlGPU = nvmlGPUs[nvmlIdx];

                    // Match by name (DXGI and NVML should report same name for same GPU)
                    if (dxgiGPU.name == nvmlGPU.name)
                    {
                        m_DXGIToNVMLMap[static_cast<uint32_t>(dxgiIdx)] = static_cast<uint32_t>(nvmlIdx);
                        spdlog::debug("WindowsGPUProbe: Mapped DXGI GPU {} to NVML GPU {} ({})", dxgiIdx, nvmlIdx, dxgiGPU.name);
                        break;
                    }
                }
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
        return;
    }

    // Get NVML counters
    auto nvmlCounters = m_NVMLProbe->readGPUCounters();
    if (nvmlCounters.empty())
    {
        return;
    }

    // Merge NVML data into DXGI counters based on mapping
    for (std::size_t dxgiIdx = 0; dxgiIdx < dxgiCounters.size(); ++dxgiIdx)
    {
        auto mapIt = m_DXGIToNVMLMap.find(static_cast<uint32_t>(dxgiIdx));
        if (mapIt == m_DXGIToNVMLMap.end())
        {
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

        // Use NVML GPU utilization if available (may be more accurate)
        if (nvmlCounter.utilizationPercent > 0.0)
        {
            dxgiCounter.utilizationPercent = nvmlCounter.utilizationPercent;
        }

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
    // Phase 3: Use D3DKMT for per-process GPU metrics (works for all vendors)
    if (m_D3DKMTProbe)
    {
        return m_D3DKMTProbe->readProcessGPUCounters();
    }
    return {};
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
