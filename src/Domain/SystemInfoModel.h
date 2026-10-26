#pragma once

#include "Domain/CrashHistory.h"
#include "Domain/PublicationSlot.h"
#include "Platform/ISystemInfoProbe.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Domain
{

/// One immutable read of the System Information page's static facts (#1399). Each section's raw
/// facts are a member; a new section adds one.
struct SystemInfoSnapshot
{
    std::uint64_t version = 0;           ///< 0 until the first read.
    std::uint64_t readAtUnixSeconds = 0; ///< When the read started (#1773).
    Platform::OsInfo os;
    Platform::FirmwareInfo firmware;
    Platform::MemoryModulesInfo memory;
    Platform::CommitPagingInfo paging;
    Platform::StorageInfo storage;
    Platform::GraphicsInfo graphics;
    Platform::PlatformSecurityInfo security;
    Platform::SensorsInfo sensors;
    Platform::DevicesInfo devices;
    Platform::DriversInfo drivers;
    Platform::CrashesInfo crashes;
    Platform::NetworkAdaptersInfo adapters;
    Platform::BootPerformanceInfo boot;
    /// This read asked the probe for the Wi-Fi details that can raise a location prompt (#1782).
    bool wifiDetailsIncluded = false;
    /// This read looked the public addresses up (#1518): the user asked, and the probe can.
    bool publicIpIncluded = false;
    /// The public addresses, when publicIpIncluded.
    Platform::PublicIpInfo publicIp;
};

/// Reads the static facts on demand (once when the page first shows, and on Refresh), never per tick,
/// and publishes each read as an immutable SystemInfoSnapshot the UI adopts by pointer.
class SystemInfoModel
{
  public:
    /// @param crashHistory The shared crash cache Process Details shows (#1675), handed each read's
    ///        crashes so it need not read them again; null for none.
    explicit SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe> probe, std::shared_ptr<CrashHistory> crashHistory = nullptr);

    /// Reads every section and publishes the result. Thread-safe; reads are serialised. Called off the
    /// UI thread. @p stopToken cancels a password prompt requestAuthorization() asked for (#2381).
    void read(const std::stop_token& stopToken = {});

    /// read(), for a worker thread: a read that throws is caught here, on the thread that threw it, and
    /// only its message is returned (a copy), so no exception object reaches another thread through a
    /// future (#1685, #1706). @return nullopt when the read succeeded; else the exception's what(), or
    /// "unknown error" for a throw that isn't a std::exception. Only building that message (bad_alloc)
    /// can still escape.
    [[nodiscard]] std::optional<std::string> tryRead(const std::stop_token& stopToken = {});

    /// The newest read (an empty version-0 one before the first).
    [[nodiscard]] std::shared_ptr<const SystemInfoSnapshot> snapshot() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// From now on, every read() asks the probe for the Wi-Fi details too, which can raise a Windows
    /// location prompt (#1782): the user clicked "Show Wi-Fi details". The choice holds for the model's
    /// life (the session). Thread-safe; takes effect from the next read() that starts.
    void includeWifiDetails() noexcept
    {
        m_IncludeWifiDetails.store(true, std::memory_order_relaxed);
    }

    /// Whether includeWifiDetails() was called.
    [[nodiscard]] bool wifiDetailsIncluded() const noexcept
    {
        return m_IncludeWifiDetails.load(std::memory_order_relaxed);
    }

    /// From now on, every read() also looks the public addresses up (#1518), which sends a query to a
    /// DNS resolver off the machine: the user clicked "Show public IP". The choice holds for the model's
    /// life (the session). Thread-safe; takes effect from the next read() that starts.
    void includePublicIp() noexcept
    {
        m_IncludePublicIp.store(true, std::memory_order_relaxed);
    }

    /// Whether includePublicIp() was called.
    [[nodiscard]] bool publicIpIncluded() const noexcept
    {
        return m_IncludePublicIp.load(std::memory_order_relaxed);
    }

    /// The next read() first asks for an administrator's password, so the facts readable by root only
    /// are read through the privileged helper (#2310): the user clicked "Authenticate to read". Once,
    /// not for the session: the helper stays authorized for a few minutes by itself. Thread-safe.
    void requestAuthorization() noexcept
    {
        m_AuthorizationRequested.store(true, std::memory_order_relaxed);
    }

    /// What the probe can read, fixed at construction, but for canReadAuthorized, which the helper can
    /// take back during a read (#2769), and a new helper gives back (#2993): that is as of the last read().
    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const
    {
        Platform::SystemInfoCapabilities capabilities = m_Capabilities;
        capabilities.canReadAuthorized = m_CanReadAuthorized.load(std::memory_order_relaxed);
        return capabilities;
    }

  private:
    std::unique_ptr<Platform::ISystemInfoProbe> m_Probe; // used under m_ReadMutex only
    Platform::SystemInfoCapabilities m_Capabilities;     // fixed at construction; canReadAuthorized's live value is below
    std::atomic<bool> m_CanReadAuthorized{false};        // refreshed by each read(), read by the UI thread (#2769)
    std::shared_ptr<CrashHistory> m_CrashHistory;        // may be null
    std::atomic<bool> m_IncludeWifiDetails{false};       // set from the UI thread, read by read() (#1782)
    std::atomic<bool> m_IncludePublicIp{false};          // set from the UI thread, read by read() (#1518)
    std::atomic<bool> m_AuthorizationRequested{false};   // set from the UI thread, taken by read() (#2310)
    std::mutex m_ReadMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_ReadMutex
    PublicationSlot<SystemInfoSnapshot> m_Slot;
};

} // namespace Domain
