#pragma once

#include "Domain/CrashHistory.h"
#include "Domain/PublicationSlot.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Domain
{

/// One immutable read of the System Information page's static facts (#1399). Each section's raw
/// facts are a member; a new section adds one.
struct SystemInfoSnapshot
{
    std::uint64_t version = 0;           ///< 0 until the first read.
    std::uint64_t readAtUnixSeconds = 0; ///< When the read was taken.
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
    /// UI thread.
    void read();

    /// read(), for a worker thread: a read that throws is caught here, on the thread that threw it, and
    /// only its message is returned (a copy), so no exception object reaches another thread through a
    /// future (#1685, #1706). @return nullopt when the read succeeded; else the exception's what(), or
    /// "unknown error" for a throw that isn't a std::exception. Only building that message (bad_alloc)
    /// can still escape.
    [[nodiscard]] std::optional<std::string> tryRead();

    /// The newest read (an empty version-0 one before the first).
    [[nodiscard]] std::shared_ptr<const SystemInfoSnapshot> snapshot() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// What the probe can read, fixed at construction.
    [[nodiscard]] const Platform::SystemInfoCapabilities& capabilities() const noexcept
    {
        return m_Capabilities;
    }

  private:
    std::unique_ptr<Platform::ISystemInfoProbe> m_Probe; // used under m_ReadMutex only
    Platform::SystemInfoCapabilities m_Capabilities;
    std::shared_ptr<CrashHistory> m_CrashHistory; // may be null
    std::mutex m_ReadMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_ReadMutex
    PublicationSlot<SystemInfoSnapshot> m_Slot;
};

} // namespace Domain
