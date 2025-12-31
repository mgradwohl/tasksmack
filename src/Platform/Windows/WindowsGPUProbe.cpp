#include "WindowsGPUProbe.h"

#include <spdlog/spdlog.h>

namespace Platform
{

std::vector<GPUInfo> WindowsGPUProbe::enumerateGPUs()
{
    // Stub implementation - returns no GPUs
    // TODO: Implement DXGI, D3DKMT, and NVML GPU enumeration
    spdlog::debug("WindowsGPUProbe: GPU enumeration not yet implemented");
    return {};
}

std::vector<GPUCounters> WindowsGPUProbe::readGPUCounters()
{
    // Stub implementation - returns empty
    // TODO: Implement GPU metrics reading via DXGI and NVML
    return {};
}

std::vector<ProcessGPUCounters> WindowsGPUProbe::readProcessGPUCounters()
{
    // Stub implementation - returns empty
    // TODO: Implement per-process GPU metrics via D3DKMT
    return {};
}

GPUCapabilities WindowsGPUProbe::capabilities() const
{
    // Stub implementation - no capabilities yet
    return GPUCapabilities{};
}

} // namespace Platform
