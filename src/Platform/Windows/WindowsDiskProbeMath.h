#pragma once

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

/// Cumulative busy time of a disk, in 100 ns units, from DISK_PERFORMANCE's QueryTime and
/// IdleTime (#1108).
///
/// QueryTime is the system time of the query and IdleTime the cumulative time the disk had
/// nothing outstanding, both in 100 ns units, so QueryTime - IdleTime grows by exactly the time
/// the disk was busy between two queries. Its absolute value is meaningless, but it is monotonic
/// and only its delta is used. Summing ReadTime and WriteTime instead counts every queued request
/// separately, so overlapping I/O (queue depth > 1) outran wall time and pinned utilisation at
/// 100 %.
///
/// @return nullopt when IdleTime is not usable: zero or negative (the driver does not track it)
///         or larger than QueryTime (inconsistent); the caller falls back to ReadTime + WriteTime.
[[nodiscard]] constexpr std::optional<std::uint64_t> diskBusyTime100ns(std::int64_t queryTime, std::int64_t idleTime) noexcept
{
    if (idleTime <= 0 || queryTime <= 0 || idleTime > queryTime)
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(queryTime - idleTime);
}

/// How often WindowsDiskProbe re-enumerates physical disks when nothing has failed, so a disk
/// added after start-up appears and one removed cleanly disappears (#1159).
inline constexpr std::chrono::seconds DISK_REENUMERATE_INTERVAL{30};

/// Whether WindowsDiskProbe should rebuild its disk list before this read (#1159).
///
/// The list used to be fixed at construction, so a removed disk failed IOCTL_DISK_PERFORMANCE on
/// every refresh forever and a newly attached one never appeared. Re-enumerate right after any
/// disk failed, and otherwise once the interval has elapsed since the last enumeration.
[[nodiscard]] inline bool shouldReenumerate(std::chrono::steady_clock::time_point now,
                                            std::chrono::steady_clock::time_point lastEnumeration,
                                            bool anyFailure,
                                            std::chrono::steady_clock::duration interval) noexcept
{
    if (anyFailure)
    {
        return true;
    }
    // A clock that went backwards (it cannot, for steady_clock, but a caller's fake clock can)
    // re-enumerates rather than waiting out an interval that never elapses.
    return now < lastEnumeration || (now - lastEnumeration) >= interval;
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
