#include "SystemInfoModel.h"

#include "Domain/CrashHistory.h"
#include "Platform/ISystemInfoProbe.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
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
    m_CanReadAuthorized.store(m_Capabilities.canReadAuthorized, std::memory_order_relaxed);
}

void SystemInfoModel::read(const std::stop_token& stopToken)
{
    const std::scoped_lock lock(m_ReadMutex);

    auto next = std::make_shared<SystemInfoSnapshot>();
    // Stamped when the read starts, not when it ends. The crash cache orders publications by its own
    // ticket, taken here, so a slow read that started earlier can't replace one that started later
    // (#1773); the wall-clock time is only shown, since it can step backwards (#2157).
    const std::uint64_t crashReadOrder = m_CrashHistory ? m_CrashHistory->beginRead() : 0;
    const auto started = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    next->readAtUnixSeconds = started > 0 ? static_cast<std::uint64_t>(started) : 0;
    if (m_AuthorizationRequested.exchange(false, std::memory_order_relaxed) && m_CanReadAuthorized.load(std::memory_order_relaxed))
    {
        // The password prompt, on this (the read's) thread; a dismissed prompt just leaves the facts marked
        static_cast<void>(m_Probe->authorize(stopToken));
    }
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
        // The Wi-Fi details that can raise a location prompt only once the user asked for them (#1782).
        next->wifiDetailsIncluded = wifiDetailsIncluded();
        next->adapters = m_Probe->readNetworkAdapters(next->wifiDetailsIncluded);
        next->boot = m_Probe->readBootPerformance();
        // The public addresses go off the machine to ask, so only once the user asked for them (#1518).
        next->publicIpIncluded = m_Capabilities.hasPublicIp && publicIpIncluded();
        if (next->publicIpIncluded)
        {
            next->publicIp = m_Probe->readPublicIp();
        }
    }
    // The helper can take "Authenticate to read" back during the reads above, once it has shown it runs
    // as another account (Windows, #2769): from this read on the page stops offering it.
    m_CanReadAuthorized.store(m_Probe->canReadAuthorized(), std::memory_order_relaxed);
    if (m_CrashHistory && m_Capabilities.hasOs)
    {
        // Process Details' crash line (#1675) shows this read too, without a second one of its own.
        m_CrashHistory->publish(next->crashes, next->readAtUnixSeconds, crashReadOrder);
    }
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

namespace
{

/// @p readFn's throw as its message, caught on the thread that threw it (#1685, #1706).
template<typename ReadFn> [[nodiscard]] std::optional<std::string> tryReading(ReadFn&& readFn)
{
    try
    {
        std::forward<ReadFn>(readFn)();
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

} // namespace

std::optional<std::string> SystemInfoModel::tryRead(const std::stop_token& stopToken)
{
    return tryReading([this, &stopToken] { read(stopToken); });
}

} // namespace Domain
