#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#if defined(__linux__) && __has_include(<unistd.h>)
#include "PosixGuards.h"

#include <array>
#include <cerrno>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace Platform::ProcParsing
{

#if defined(__linux__) && __has_include(<unistd.h>)

/// Read a /proc or /sys virtual file using low-level POSIX I/O.
/// Avoids std::ifstream overhead (locale machinery, sentry, streambuf allocations).
/// Loops to guard against short reads (POSIX allows ::read to return less than requested).
/// `path` is resolved relative to the directory `dirFd` is open on (openat(2)), or as an ordinary
/// path when dirFd is AT_FDCWD. Reading a process's files through one handle on its /proc/[pid]
/// directory costs one path lookup per file instead of a walk from the root, and keeps every read
/// on that process even if its PID is reused meanwhile (#1336).
/// Returns bytes read, or 0 on failure. Buffer is NOT null-terminated.
[[nodiscard]] inline std::size_t readProcFileAt(int dirFd, const char* path, char* buf, std::size_t bufSize) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX openat() is variadic
    const int fd = ::openat(dirFd, path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return 0;
    }
    std::size_t total = 0;
    bool readError = false;
    while (total < bufSize)
    {
        const auto n = ::read(fd, buf + total, bufSize - total);
        if (n == 0)
        {
            break; // EOF
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // interrupted by signal — retry
            }
            readError = true;
            break; // I/O error — discard partial data
        }
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return readError ? 0 : total;
}

/// readProcFileAt() with a single read(2): for a per-process /proc file generated whole by the kernel
/// (a seq_file: stat, statm, status, io, cgroup), one read returns all of it that fits in the buffer
/// -- a read comes back short only at the end of the file -- so the read that would only confirm the
/// end is skipped. That is one syscall less per file, for every process on every sample. A result of
/// bufSize bytes may be cut off; callers that need the whole file read it again in full. Not for
/// files read in pages (cmdline, environ) or any that can come back short before their end.
[[nodiscard]] inline std::size_t readProcFileOnceAt(int dirFd, const char* path, char* buf, std::size_t bufSize) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX openat() is variadic
    const int fd = ::openat(dirFd, path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return 0;
    }
    ssize_t n = ::read(fd, buf, bufSize);
    while (n < 0 && errno == EINTR)
    {
        n = ::read(fd, buf, bufSize); // interrupted by signal — retry
    }
    ::close(fd);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

/// readProcFileAt() for an ordinary path.
[[nodiscard]] inline std::size_t readProcFile(const char* path, char* buf, std::size_t bufSize) noexcept
{
    return readProcFileAt(AT_FDCWD, path, buf, bufSize);
}

/// Read an entire /proc or /sys virtual file into a heap buffer, growing until EOF.
/// Avoids the fixed-size truncation of readProcFile for files without a known upper bound.
/// `path` is resolved as in readProcFileAt(). Returns the bytes read, or an empty vector on failure.
[[nodiscard]] inline std::vector<char> readProcFileFullAt(int dirFd, const char* path)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX openat() is variadic
    const int fd = ::openat(dirFd, path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return {};
    }
    const Posix::FdGuard guard{fd}; // ensures fd is closed on all paths, including exception paths
                                    // (buf.reserve / buf.insert can throw on OOM)

    std::vector<char> buf;
    buf.reserve(4096);
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const auto n = ::read(fd, chunk.data(), chunk.size());
        if (n == 0)
        {
            break; // EOF
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // interrupted by signal — retry
            }
            return {}; // I/O error — discard partial data
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + static_cast<std::size_t>(n));
    }
    return buf;
}

/// readProcFileFullAt() for an ordinary path.
[[nodiscard]] inline std::vector<char> readProcFileFull(const char* path)
{
    return readProcFileFullAt(AT_FDCWD, path);
}

#endif

/// Skip ASCII space/tab characters, returning the updated pointer.
[[nodiscard]] constexpr const char* skipSpaces(const char* p, const char* end) noexcept
{
    while (p < end && (*p == ' ' || *p == '\t'))
    {
        ++p;
    }
    return p;
}

/// Parse one integer of type T from [p, end), skipping leading spaces.
/// Advances p past the parsed digits on success; leaves p unchanged and returns false on failure.
template<std::integral T> bool parseNum(const char*& p, const char* end, T& out) noexcept
{
    const char* const saved = p;
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        p = saved;
        return false;
    }
    p = ptr;
    return true;
}

/// Parse one double from [p, end), skipping leading spaces.
/// Advances p past the parsed number on success; leaves p unchanged and returns false on failure.
inline bool parseDouble(const char*& p, const char* end, double& out) noexcept
{
    const char* const saved = p;
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        p = saved;
        return false;
    }
    p = ptr;
    return true;
}

/// The fields TaskSmack reads from a /proc/[pid]/stat line, by their proc(5) field numbers.
struct StatFields
{
    std::string_view comm;          ///< (2) without its parentheses; a view into the parsed line
    char state = '?';               ///< (3)
    std::int32_t parentPid = 0;     ///< (4)
    std::uint64_t minorFaults = 0;  ///< (10) minflt
    std::uint64_t majorFaults = 0;  ///< (12) majflt
    std::uint64_t userTime = 0;     ///< (14) utime, clock ticks
    std::uint64_t systemTime = 0;   ///< (15) stime, clock ticks
    std::int64_t nice = 0;          ///< (19)
    std::int64_t numThreads = 0;    ///< (20)
    std::uint64_t startTime = 0;    ///< (22) starttime, clock ticks after boot
    std::uint64_t virtualBytes = 0; ///< (23) vsize
    std::int64_t rssPages = 0;      ///< (24) rss
};

namespace Detail
{

/// True if @p c separates /proc/[pid]/stat fields.
[[nodiscard]] constexpr bool isStatSeparator(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n';
}

/// parseNum() for a /proc/[pid]/stat field: the number must also end at a separator or the end of
/// the line, so "7-8" is not read as the two fields 7 and -8, nor "9x" as 9. parseNum() itself stays
/// prefix-based for the readers of unit-suffixed values ("1234 kB"). Leaves p unchanged on failure.
template<std::integral T> bool parseStatNum(const char*& p, const char* end, T& out) noexcept
{
    const char* const saved = p;
    if (!parseNum(p, end, out) || (p < end && !isStatSeparator(*p)))
    {
        p = saved;
        return false;
    }
    return true;
}

/// How far parseStatPrefix() reads: to the start time, or on through rss.
enum class StatParseExtent : std::uint8_t
{
    ThroughStartTime,
    ThroughRss,
};

/// Parse a /proc/[pid]/stat line into @p out up to @p extent; false if it is malformed.
///
/// comm (2) is the executable name as the process set it and may itself contain spaces and
/// parentheses, so it runs from the first '(' to the *last* ')' and the numbered fields are counted
/// from there: a crafted name cannot shift which number is read as which field (#973). The state
/// must be followed by a separator, and every field read must be a whole number of its type that
/// ends at a separator or the end of the line.
[[nodiscard]] inline bool parseStatPrefix(std::string_view line, StatFields& out, StatParseExtent extent) noexcept
{
    const std::size_t commOpen = line.find('(');
    const std::size_t commClose = line.rfind(')');
    if (commOpen == std::string_view::npos || commClose == std::string_view::npos || commClose <= commOpen)
    {
        return false;
    }
    // Built from pointer and length rather than substr(), which may throw; both offsets are in range.
    out.comm = std::string_view(line.data() + commOpen + 1, commClose - commOpen - 1);

    const char* p = line.data() + commClose + 1;
    const char* const end = line.data() + line.size();
    p = skipSpaces(p, end);
    if (p >= end)
    {
        return false;
    }
    out.state = *p++;
    if (p >= end || !isStatSeparator(*p))
    {
        return false;
    }

    // Fields 5-9, 11, 13, 16-18 and 21 are read only to step over them.
    std::int32_t pgrp = 0;
    std::int32_t session = 0;
    std::int32_t ttyNr = 0;
    std::int32_t tpgid = 0;
    std::uint32_t flags = 0;
    std::uint64_t cminflt = 0;
    std::uint64_t cmajflt = 0;
    std::int64_t cutime = 0;
    std::int64_t cstime = 0;
    std::int64_t priority = 0;
    std::int64_t itrealvalue = 0;

    // clang-format off
    if (!parseStatNum(p, end, out.parentPid)   || !parseStatNum(p, end, pgrp)            ||
        !parseStatNum(p, end, session)         || !parseStatNum(p, end, ttyNr)           ||
        !parseStatNum(p, end, tpgid)           || !parseStatNum(p, end, flags)           ||
        !parseStatNum(p, end, out.minorFaults) || !parseStatNum(p, end, cminflt)         ||
        !parseStatNum(p, end, out.majorFaults) || !parseStatNum(p, end, cmajflt)         ||
        !parseStatNum(p, end, out.userTime)    || !parseStatNum(p, end, out.systemTime)  ||
        !parseStatNum(p, end, cutime)          || !parseStatNum(p, end, cstime)          ||
        !parseStatNum(p, end, priority)        || !parseStatNum(p, end, out.nice)        ||
        !parseStatNum(p, end, out.numThreads)  || !parseStatNum(p, end, itrealvalue)     ||
        !parseStatNum(p, end, out.startTime))
    // clang-format on
    {
        return false;
    }
    if (extent == StatParseExtent::ThroughStartTime)
    {
        return true;
    }
    return parseStatNum(p, end, out.virtualBytes) && parseStatNum(p, end, out.rssPages);
}

} // namespace Detail

/// The fields of a /proc/[pid]/stat line through rss (24), or nullopt if it is malformed. The one
/// parser of that format (#1183): LinuxProcessProbe's values, the identity check before a process
/// action (#973) and the start time socket attribution compares (#1336) all count fields the same way.
[[nodiscard]] inline std::optional<StatFields> parseStatFields(std::string_view line) noexcept
{
    StatFields fields;
    if (!Detail::parseStatPrefix(line, fields, Detail::StatParseExtent::ThroughRss))
    {
        return std::nullopt;
    }
    return fields;
}

/// The start time (field 22, clock ticks since boot) of a /proc/[pid]/stat line -- the value
/// LinuxProcessProbe reports as ProcessCounters::startTimeTicks -- or nullopt if the line is
/// malformed. Read the same way as parseStatFields(), but needs nothing after the start time.
[[nodiscard]] inline std::optional<std::uint64_t> parseStatStartTime(std::string_view line) noexcept
{
    StatFields fields;
    if (!Detail::parseStatPrefix(line, fields, Detail::StatParseExtent::ThroughStartTime))
    {
        return std::nullopt;
    }
    return fields.startTime;
}

} // namespace Platform::ProcParsing
