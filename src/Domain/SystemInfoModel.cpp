#include "SystemInfoModel.h"

#include "Domain/CrashHistory.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Domain
{

SystemInfoModel::SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe> probe, std::shared_ptr<CrashHistory> crashHistory)
    : m_Probe(std::move(probe)), m_CrashHistory(std::move(crashHistory))
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
        next->devices = m_Probe->readDevices();
        next->drivers = m_Probe->readDrivers();
        next->crashes = m_Probe->readCrashes();
        next->adapters = m_Probe->readNetworkAdapters();
        next->boot = m_Probe->readBootPerformance();
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    next->readAtUnixSeconds = static_cast<std::uint64_t>(now);
    if (m_CrashHistory && m_Capabilities.hasOs)
    {
        // Process Details' crash line (#1675) shows this read too, without a second one of its own.
        m_CrashHistory->publish(next->crashes, next->readAtUnixSeconds);
    }
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

std::optional<std::string> SystemInfoModel::tryRead()
{
    try
    {
        read();
    }
    catch (const std::exception& e)
    {
        return std::string(e.what());
    }
    catch (...)
    {
        return std::string("unknown error");
    }
    return std::nullopt;
}

} // namespace Domain
