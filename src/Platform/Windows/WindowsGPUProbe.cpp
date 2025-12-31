#include "WindowsGPUProbe.h"

#include "DXGIGPUProbe.h"

#include <spdlog/spdlog.h>

namespace Platform
{

WindowsGPUProbe::WindowsGPUProbe() : m_DXGIProbe(std::make_unique<DXGIGPUProbe>())
{
    spdlog::debug("WindowsGPUProbe: Initialized with DXGI probe");
}

std::vector<GPUInfo> WindowsGPUProbe::enumerateGPUs()
{
    if (m_DXGIProbe)
    {
        return m_DXGIProbe->enumerateGPUs();
    }
    return {};
}

std::vector<GPUCounters> WindowsGPUProbe::readGPUCounters()
{
    if (m_DXGIProbe)
    {
        return m_DXGIProbe->readGPUCounters();
    }
    return {};
}

std::vector<ProcessGPUCounters> WindowsGPUProbe::readProcessGPUCounters()
{
    if (m_DXGIProbe)
    {
        return m_DXGIProbe->readProcessGPUCounters();
    }
    return {};
}

GPUCapabilities WindowsGPUProbe::capabilities() const
{
    if (m_DXGIProbe)
    {
        return m_DXGIProbe->capabilities();
    }
    return GPUCapabilities{};
}

} // namespace Platform
