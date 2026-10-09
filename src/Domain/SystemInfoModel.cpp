#include "SystemInfoModel.h"

#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

SystemInfoModel::SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe> probe) : m_Probe(std::move(probe))
{
    if (!m_Probe)
    {
        throw std::invalid_argument("SystemInfoModel requires a probe");
    }
    m_Capabilities = m_Probe->capabilities();
}

void SystemInfoModel::read()
{
    const std::scoped_lock lock(m_ReadMutex);

    auto next = std::make_shared<SystemInfoSnapshot>();
    if (m_Capabilities.hasOs)
    {
        next->os = m_Probe->readOs();
        next->firmware = m_Probe->readFirmware();
        next->memory = m_Probe->readMemoryModules();
        next->paging = m_Probe->readCommitPaging();
        next->storage = m_Probe->readStorage();
        next->graphics = m_Probe->readGraphics();
        next->security = m_Probe->readPlatformSecurity();
        next->sensors = m_Probe->readSensors();
        next->adapters = m_Probe->readNetworkAdapters();
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    next->readAtUnixSeconds = static_cast<std::uint64_t>(now);
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
