#include "LinuxGPUProbe.h"

#include "DRMGPUProbe.h"
#include "NVMLGPUProbe.h"

#include <spdlog/spdlog.h>

namespace Platform
{

LinuxGPUProbe::LinuxGPUProbe()
    : m_NVMLProbe(std::make_unique<NVMLGPUProbe>()), m_DRMProbe(std::make_unique<DRMGPUProbe>())
{
    std::vector<std::string> probes;
    if (m_NVMLProbe->isAvailable())
    {
        probes.push_back("NVML");
    }
    if (m_DRMProbe->isAvailable())
    {
        probes.push_back("DRM");
    }

    std::string probeSummary = probes.empty() ? "None" : "";
    for (std::size_t i = 0; i < probes.size(); ++i)
    {
        probeSummary += probes[i];
        if (i < probes.size() - 1)
        {
            probeSummary += " + ";
        }
    }

    spdlog::debug("LinuxGPUProbe: Initialized with {} probe(s)", probeSummary);
}

std::vector<GPUInfo> LinuxGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    // Phase 4: Try NVML first (NVIDIA)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlGPUs = m_NVMLProbe->enumerateGPUs();
        gpus.insert(gpus.end(), nvmlGPUs.begin(), nvmlGPUs.end());
    }

    // Phase 5: Try DRM (Intel)
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        auto drmGPUs = m_DRMProbe->enumerateGPUs();
        gpus.insert(gpus.end(), drmGPUs.begin(), drmGPUs.end());
    }

    // Future: Add ROCm (AMD)

    if (gpus.empty())
    {
        spdlog::debug("LinuxGPUProbe: No GPU vendor libraries available");
    }

    return gpus;
}

std::vector<GPUCounters> LinuxGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    // Phase 4: NVML metrics for NVIDIA GPUs
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCounters = m_NVMLProbe->readGPUCounters();
        counters.insert(counters.end(), nvmlCounters.begin(), nvmlCounters.end());
    }

    // Phase 5: DRM metrics for Intel GPUs
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        auto drmCounters = m_DRMProbe->readGPUCounters();
        counters.insert(counters.end(), drmCounters.begin(), drmCounters.end());
    }

    // Future: Add ROCm

    return counters;
}

std::vector<ProcessGPUCounters> LinuxGPUProbe::readProcessGPUCounters()
{
    std::vector<ProcessGPUCounters> counters;

    // Phase 4: NVML per-process metrics for NVIDIA
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        auto nvmlCounters = m_NVMLProbe->readProcessGPUCounters();
        counters.insert(counters.end(), nvmlCounters.begin(), nvmlCounters.end());
    }

    // Phase 5: DRM per-process metrics (not available via sysfs)
    // Future: Add vendor-specific per-process metrics

    return counters;
}

GPUCapabilities LinuxGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Phase 4: NVML capabilities
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

    // Phase 5: DRM capabilities (Intel)
    if (m_DRMProbe && m_DRMProbe->isAvailable())
    {
        auto drmCaps = m_DRMProbe->capabilities();

        caps.hasTemperature = caps.hasTemperature || drmCaps.hasTemperature;
        caps.hasHotspotTemp = caps.hasHotspotTemp || drmCaps.hasHotspotTemp;
        caps.hasPowerMetrics = caps.hasPowerMetrics || drmCaps.hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds || drmCaps.hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed || drmCaps.hasFanSpeed;
        caps.hasPCIeMetrics = caps.hasPCIeMetrics || drmCaps.hasPCIeMetrics;
        caps.hasEngineUtilization = caps.hasEngineUtilization || drmCaps.hasEngineUtilization;
        caps.hasPerProcessMetrics = caps.hasPerProcessMetrics || drmCaps.hasPerProcessMetrics;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder || drmCaps.hasEncoderDecoder;
        caps.supportsMultiGPU = caps.supportsMultiGPU || drmCaps.supportsMultiGPU;
    }

    // Future: Merge capabilities from ROCm

    return caps;
}

} // namespace Platform
