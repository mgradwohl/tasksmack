#include "LinuxGPUProbe.h"

#include "NVMLGPUProbe.h"

#include <spdlog/spdlog.h>

namespace Platform
{

LinuxGPUProbe::LinuxGPUProbe() : m_NVMLProbe(std::make_unique<NVMLGPUProbe>())
{
    std::string probeSummary = "None";
    if (m_NVMLProbe->isAvailable())
    {
        probeSummary = "NVML";
    }
    spdlog::debug("LinuxGPUProbe: Initialized with {} probe(s)", probeSummary);
}

std::vector<GPUInfo> LinuxGPUProbe::enumerateGPUs()
{
    // Phase 4: Try NVML first (NVIDIA)
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        return m_NVMLProbe->enumerateGPUs();
    }

    // Future: Add ROCm (AMD), DRM (Intel/generic)
    spdlog::debug("LinuxGPUProbe: No GPU vendor libraries available");
    return {};
}

std::vector<GPUCounters> LinuxGPUProbe::readGPUCounters()
{
    // Phase 4: NVML metrics for NVIDIA GPUs
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        return m_NVMLProbe->readGPUCounters();
    }

    // Future: Add ROCm, DRM
    return {};
}

std::vector<ProcessGPUCounters> LinuxGPUProbe::readProcessGPUCounters()
{
    // Phase 4: NVML per-process metrics for NVIDIA
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        return m_NVMLProbe->readProcessGPUCounters();
    }

    // Future: Add vendor-specific per-process metrics
    return {};
}

GPUCapabilities LinuxGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // Phase 4: NVML capabilities
    if (m_NVMLProbe && m_NVMLProbe->isAvailable())
    {
        caps = m_NVMLProbe->capabilities();
    }

    // Future: Merge capabilities from ROCm, DRM

    return caps;
}

} // namespace Platform
