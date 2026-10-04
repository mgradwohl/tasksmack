#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Platform
{

/// IOCTL_DISK_PERFORMANCE's counter fields (BytesRead/BytesWritten/ReadTime/WriteTime) come
/// from the signed LARGE_INTEGER::QuadPart member. A buggy or virtualized disk driver
/// (VM/WSL passthrough, some RAID controllers) reporting a negative value would otherwise
/// wrap to near UINT64_MAX when cast directly to uint64_t, producing a spurious
/// multi-exabyte rate spike in StorageModel's next delta-based computation. Treat a
/// negative value as "no data this sample" rather than reinterpreting the sign bit as
/// magnitude.
[[nodiscard]] inline uint64_t clampNonNegativeQuadPart(int64_t value)
{
    return value < 0 ? 0ULL : static_cast<uint64_t>(value);
}

/// PDH's PhysicalDisk instance names are formatted "<index> <driveletter(s)>", e.g.
/// "0 C:" or "1 D: E:" for a disk backing multiple volumes. Extract the leading index
/// so we can open the matching \\.\PhysicalDriveN device directly.
[[nodiscard]] inline std::optional<int> parsePhysicalDriveIndex(std::wstring_view instanceName)
{
    const auto spacePos = instanceName.find(L' ');
    const std::wstring_view indexPart = (spacePos == std::wstring_view::npos) ? instanceName : instanceName.substr(0, spacePos);
    if (indexPart.empty())
    {
        return std::nullopt;
    }

    int index = 0;
    for (const wchar_t ch : indexPart)
    {
        if (ch < L'0' || ch > L'9')
        {
            return std::nullopt;
        }
        const int digit = ch - L'0';
        // An implausibly long numeric prefix would otherwise overflow signed int here
        // (undefined behavior) and could return an arbitrary drive index; fail closed instead.
        if (index > (std::numeric_limits<int>::max() - digit) / 10)
        {
            return std::nullopt;
        }
        index = (index * 10) + digit;
    }
    return index;
}

/// Per-disk state for advanceDiskBusy(): the baseline its busy time is measured from.
struct DiskBusyClock
{
    std::int64_t baseElapsed100ns = 0; ///< Monotonic time at the baseline
    std::int64_t baseIdle100ns = 0;    ///< IdleTime at the baseline
    std::uint64_t baseBusy100ns = 0;   ///< Busy time carried over from before the baseline
    std::uint64_t lastBusy100ns = 0;   ///< Last value returned; the result never goes below it
    std::int64_t lastIdle100ns = 0;
    bool started = false;
};

/// Cumulative busy time of a disk, in 100 ns units, from DISK_PERFORMANCE's IdleTime (#1108).
///
/// IdleTime is the cumulative time the disk had nothing outstanding, so between two reads the disk
/// was busy for the elapsed time less the growth in IdleTime. Summing ReadTime and WriteTime
/// instead counts every queued request separately, so overlapping I/O (queue depth > 1) outran
/// wall time and pinned utilisation at 100 %.
///
/// The elapsed time is the caller's monotonic clock, not DISK_PERFORMANCE's QueryTime: QueryTime is
/// adjustable system time, so a clock correction would have read as disk activity (a +5 s step
/// turned a 60 %-busy second into 100 %) and a backward one as idleness. The result is measured
/// from a baseline (the first read, or the read after IdleTime or the clock went backwards, e.g. a
/// counter reset) and never decreases, so StorageModel's delta over its own elapsed time is the
/// busy fraction.
///
/// @param elapsed100ns  The caller's monotonic clock now, in 100 ns units.
/// @param idle100ns     DISK_PERFORMANCE.IdleTime.
/// @return nullopt when IdleTime is not usable (zero or negative: the driver does not track it);
///         the caller falls back to ReadTime + WriteTime.
[[nodiscard]] constexpr std::optional<std::uint64_t>
advanceDiskBusy(DiskBusyClock& clock, std::int64_t elapsed100ns, std::int64_t idle100ns) noexcept
{
    if (idle100ns <= 0)
    {
        return std::nullopt;
    }
    if (!clock.started || idle100ns < clock.lastIdle100ns || elapsed100ns < clock.baseElapsed100ns)
    {
        clock.baseElapsed100ns = elapsed100ns;
        clock.baseIdle100ns = idle100ns;
        clock.baseBusy100ns = clock.lastBusy100ns;
        clock.started = true;
    }
    clock.lastIdle100ns = idle100ns;

    const std::int64_t span = elapsed100ns - clock.baseElapsed100ns;
    const std::int64_t idleSpan = idle100ns - clock.baseIdle100ns;
    const std::uint64_t busySinceBase = span > idleSpan ? static_cast<std::uint64_t>(span - idleSpan) : 0U;
    clock.lastBusy100ns = std::max(clock.lastBusy100ns, clock.baseBusy100ns + busySinceBase);
    return clock.lastBusy100ns;
}

/// How often WindowsDiskProbe re-enumerates physical disks when nothing has failed, so a disk
/// added after start-up appears and one removed cleanly disappears (#1159).
inline constexpr std::chrono::seconds DISK_REENUMERATE_INTERVAL{30};

/// The shortest gap between re-enumerations after a disk failed (#1159). A removed disk is
/// dropped on the next read, but a disk that opens yet fails every IOCTL_DISK_PERFORMANCE read
/// would otherwise re-run the PDH enumeration and reopen every drive on every refresh.
inline constexpr std::chrono::seconds DISK_REENUMERATE_AFTER_FAILURE_INTERVAL{5};

/// Whether WindowsDiskProbe should rebuild its disk list before this read (#1159).
///
/// The list used to be fixed at construction, so a removed disk failed IOCTL_DISK_PERFORMANCE on
/// every refresh forever and a newly attached one never appeared. Re-enumerate after a disk
/// failed, at most once per DISK_REENUMERATE_AFTER_FAILURE_INTERVAL, and otherwise once
/// `interval` has elapsed since the last enumeration.
[[nodiscard]] inline bool shouldReenumerate(std::chrono::steady_clock::time_point now,
                                            std::chrono::steady_clock::time_point lastEnumeration,
                                            bool anyFailure,
                                            std::chrono::steady_clock::duration interval) noexcept
{
    // A clock that went backwards (it cannot, for steady_clock, but a caller's fake clock can)
    // re-enumerates rather than waiting out an interval that never elapses.
    if (now < lastEnumeration)
    {
        return true;
    }
    const auto sinceLast = now - lastEnumeration;
    return sinceLast >= interval ||
           (anyFailure && sinceLast >= std::min<std::chrono::steady_clock::duration>(interval, DISK_REENUMERATE_AFTER_FAILURE_INTERVAL));
}

/// Tracks which keys (disk names, device paths) are currently failing, so a failure is logged as a
/// warning once and at debug level while it persists, instead of a warning every refresh (#1159).
class FailureLogLimiter
{
  public:
    /// Record a failure for `key`. @return true if this is its first failure since it last
    /// succeeded (log a warning), false if it was already failing (log at debug level).
    [[nodiscard]] bool recordFailure(const std::string& key)
    {
        return m_Failing.insert(key).second;
    }

    /// Record a success for `key`, so its next failure warns again.
    void recordSuccess(const std::string& key)
    {
        m_Failing.erase(key);
    }

    [[nodiscard]] bool isFailing(const std::string& key) const
    {
        return m_Failing.contains(key);
    }

  private:
    std::unordered_set<std::string> m_Failing;
};

} // namespace Platform
