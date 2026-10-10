#pragma once

// The recent crashes and hangs (#1524), shared between the System tab and Process Details (#1675):
// one cache of Platform::CrashesInfo, published as an immutable snapshot. The System Information read
// publishes its crashes here as it reads them, and Process Details asks for a read of its own, on a
// worker, only while its crash line is drawn and the cache is older than
// Sampling::PROCESS_CRASH_HISTORY_REFRESH_MS: never per frame and never per selection change. The
// matching of one executable against the list is here too, pure, so it is tested without a log.

#include "Domain/PublicationSlot.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace Domain
{

/// One read of the recent crashes and hangs.
struct CrashHistorySnapshot
{
    std::uint64_t version = 0;           ///< 0 until the first read.
    std::uint64_t readAtUnixSeconds = 0; ///< When the read was taken.
    Platform::CrashesInfo crashes;
};

/// The shared cache. Thread-safe: read() runs on a worker, publish() on whichever thread read the
/// crashes, snapshot() and version() on the UI thread.
class CrashHistory
{
  public:
    /// Reads the crashes: the composition root's ISystemInfoProbe::readCrashes() (tests: a fake).
    using ReadFn = std::function<Platform::CrashesInfo()>;

    /// @throws std::invalid_argument for an empty @p read.
    explicit CrashHistory(ReadFn read);

    /// Reads the crashes and publishes them. Off the UI thread; reads are serialised. A read that
    /// throws is published as an unlisted CrashesInfo carrying the message, so nothing crosses threads
    /// but a plain value (#1685).
    void read();

    /// Publishes @p crashes, read at @p readAtUnixSeconds: the System Information read's (#1399), so
    /// Process Details shows what the System tab shows without reading the log again. A read older
    /// than the one already published is dropped.
    void publish(Platform::CrashesInfo crashes, std::uint64_t readAtUnixSeconds);

    /// The newest read (an empty version-0 one before the first).
    [[nodiscard]] std::shared_ptr<const CrashHistorySnapshot> snapshot() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

  private:
    ReadFn m_Read;                         // called under m_ReadMutex only
    std::mutex m_ReadMutex;                // one read() at a time
    std::mutex m_PublishMutex;             // orders publications from read() and from the System Information read
    std::uint64_t m_LastVersion = 0;       // guarded by m_PublishMutex
    std::uint64_t m_LastReadAtSeconds = 0; // guarded by m_PublishMutex
    PublicationSlot<CrashHistorySnapshot> m_Slot;
};

/// How many of an executable's newest crashes and hangs Process Details lists in its tooltip.
inline constexpr std::size_t CRASH_RECENT_MAX = 5;

/// The kernel's limit on a Linux process's comm, which names its core dump: longer names are cut to
/// this many bytes (TASK_COMM_LEN - 1).
inline constexpr std::size_t LINUX_COMM_MAX_BYTES = 15;

/// One executable's crashes and hangs in a CrashesInfo.
struct ExecutableCrashCounts
{
    std::size_t crashes = 0;
    std::size_t hangs = 0;
    std::uint64_t lastUnixSeconds = 0;        ///< The newest one's time; 0 when none (or unknown)
    std::vector<Platform::CrashEvent> recent; ///< Newest first, at most CRASH_RECENT_MAX
    /// Linux: the name is longer than a comm, so it was matched on its first LINUX_COMM_MAX_BYTES bytes
    /// (and could match another program sharing them).
    bool truncatedName = false;

    [[nodiscard]] std::size_t total() const noexcept
    {
        return crashes + hangs;
    }
};

/// Whether @p event is of @p executable (a file name, or a path whose file name is used). Windows:
/// the event's application, case-insensitively. Linux: the event's comm against the name's first
/// LINUX_COMM_MAX_BYTES bytes, or the journal's executable path's file name against the whole name.
/// Otherwise an exact file-name match.
[[nodiscard]] bool crashMatchesExecutable(const Platform::CrashEvent& event, std::string_view executable, Platform::OsFamily family);

/// The crashes and hangs of @p executable in @p crashes (newest first, as the probe lists them).
[[nodiscard]] ExecutableCrashCounts crashCountsFor(const Platform::CrashesInfo& crashes, std::string_view executable);

} // namespace Domain
