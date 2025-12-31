#include "LinuxGPUProbe.h"

#include <spdlog/spdlog.h>

namespace Platform
{

std::vector<GPUInfo> LinuxGPUProbe::enumerateGPUs()
{
    // Stub implementation - returns no GPUs
    // TODO: Implement NVML, ROCm, and DRM GPU enumeration
    spdlog::debug("LinuxGPUProbe: GPU enumeration not yet implemented");
    return {};
}

std::vector<GPUCounters> LinuxGPUProbe::readGPUCounters()
{
    // Stub implementation - returns empty
    // TODO: Implement GPU metrics reading
    return {};
}

std::vector<ProcessGPUCounters> LinuxGPUProbe::readProcessGPUCounters()
{
    // Stub implementation - returns empty
    // TODO: Implement per-process GPU metrics
    return {};
}

GPUCapabilities LinuxGPUProbe::capabilities() const
{
    // Stub implementation - no capabilities yet
    return GPUCapabilities{};
}

} // namespace Platform
