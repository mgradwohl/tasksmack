// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<pwd.h>) && __has_include(<unistd.h>)

#include "LinuxProcessProbe.h"

#include "Domain/SamplingConfig.h"
#include "Platform/PlatformConfig.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "NetlinkSocketStats.h"
#endif

#include "Platform/ProcessTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>

namespace Platform
{

namespace
{

[[nodiscard]] constexpr auto clampToI32(int64_t value) noexcept -> int32_t
{
    if (value < std::numeric_limits<int32_t>::min())
    {
        return std::numeric_limits<int32_t>::min();
    }
    if (value > std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }

    // Explicit narrowing is safe after range checks above.
    return static_cast<int32_t>(value);
}

template<std::integral T> [[nodiscard]] constexpr auto toU64PositiveOr(T value, uint64_t fallback) noexcept -> uint64_t
{
    if constexpr (std::is_signed_v<T>)
    {
        if (value <= 0)
        {
            return fallback;
        }
    }
    else
    {
        if (value == 0)
        {
            return fallback;
        }
    }

    // Explicit conversion: keeps -Wconversion/-Wsign-conversion happy, and callers have already ensured value is positive.
    return static_cast<uint64_t>(value);
}

/// Read a /proc or /sys virtual file using low-level POSIX I/O.
/// Avoids std::ifstream overhead (locale machinery, sentry, streambuf allocations).
/// Returns bytes read, or 0 on failure. Buffer is NOT null-terminated.
[[nodiscard]] std::size_t readProcFile(const char* path, char* buf, std::size_t bufSize) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return 0;
    }
    const auto n = ::read(fd, buf, bufSize);
    ::close(fd);
    return (n > 0) ? static_cast<std::size_t>(n) : 0;
}

/// Read an entire /proc or /sys virtual file into a heap buffer, growing until EOF.
/// Avoids the fixed-size truncation of readProcFile for files without a known upper bound.
/// Returns the bytes read, or an empty vector on failure.
[[nodiscard]] std::vector<char> readProcFileFull(const char* path)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return {};
    }
    std::vector<char> buf;
    buf.reserve(4096);
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const auto n = ::read(fd, chunk.data(), chunk.size());
        if (n <= 0)
        {
            break;
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + static_cast<std::size_t>(n));
    }
    ::close(fd);
    return buf;
}

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
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        return false;
    }
    p = ptr;
    return true;
}

/// Cache UID to username mappings to avoid repeated getpwuid calls
std::unordered_map<uid_t, std::string>& getUsernameCache()
{
    static std::unordered_map<uid_t, std::string> cache;
    return cache;
}

/// Mutex to protect the username cache
std::mutex& getUsernameCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// Get username from UID, with caching
[[nodiscard]] std::string getUsername(uid_t uid)
{
    const std::scoped_lock lock(getUsernameCacheMutex());
    auto& cache = getUsernameCache();
    auto it = cache.find(uid);
    if (it != cache.end())
    {
        return it->second;
    }

    // Look up username from passwd database (thread-safe version)
    struct passwd pwBuf = {};
    struct passwd* pwResult = nullptr;
    std::array<char, 1024> buffer{};
    std::string username;
    if (getpwuid_r(uid, &pwBuf, buffer.data(), buffer.size(), &pwResult) == 0 && pwResult != nullptr && pwResult->pw_name != nullptr)
    {
        username = pwResult->pw_name;
    }
    else
    {
        // Fall back to UID as string
        username = std::to_string(uid);
    }

    cache[uid] = username;
    return username;
}

} // namespace

LinuxProcessProbe::LinuxProcessProbe() : LinuxProcessProbe(std::filesystem::path("/proc"))
{}

LinuxProcessProbe::LinuxProcessProbe(std::filesystem::path procRoot)
    : m_ProcRoot(std::move(procRoot)),
      m_TicksPerSecond(sysconf(_SC_CLK_TCK)),
      m_PageSize(toU64PositiveOr(sysconf(_SC_PAGESIZE), 4096ULL)),
      m_BootTimeEpoch(readBootTime(m_ProcRoot))
{
    if (m_TicksPerSecond <= 0)
    {
        // /proc process times are typically reported in user-space clock ticks (USER_HZ),
        // and 100 Hz is the conventional Linux fallback for CLK_TCK in that context.
        // See 'man 7 time' and 'man 5 proc' for procfs/USER_HZ semantics.
        m_TicksPerSecond = 100;
        spdlog::warn("Failed to get CLK_TCK, using default: {}", m_TicksPerSecond);
    }

    if (m_BootTimeEpoch == 0)
    {
        spdlog::warn("Failed to read boot time from {}", (m_ProcRoot / "stat").string());
    }

    // Detect and initialize power monitoring if available
    m_HasPowerCap = detectPowerCap();
    if (m_HasPowerCap)
    {
        spdlog::info("Power monitoring available via RAPL at: {}", m_PowerCapPath);
    }
    else
    {
        spdlog::debug("Power monitoring not available (RAPL not found)");
    }

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    // Initialize per-process network monitoring via Netlink INET_DIAG
    m_SocketStats = std::make_unique<NetlinkSocketStats>();
    m_HasNetworkCounters = m_SocketStats->isAvailable();
    if (m_HasNetworkCounters)
    {
        spdlog::info("Per-process network monitoring available via Netlink INET_DIAG");
    }
    else
    {
        spdlog::debug("Per-process network monitoring not available");
    }
#endif
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
void LinuxProcessProbe::setSocketStatsCacheTtl(std::chrono::milliseconds ttlMs)
{
    if (m_SocketStats)
    {
        // Recreate with new TTL
        m_SocketStats = std::make_unique<NetlinkSocketStats>(ttlMs);
    }
}
#endif

std::vector<ProcessCounters> LinuxProcessProbe::enumerate()
{
    std::vector<ProcessCounters> processes;
    // No upfront reserve: the vector grows via amortized doubling.
    // Reserving a fixed constant (e.g. 500) wastes memory on light systems
    // and still reallocates on busy ones. Let the allocator manage growth.

    const std::filesystem::path& procPath = m_ProcRoot;
    std::error_code errorCode;

    for (const auto& entry : std::filesystem::directory_iterator(procPath, errorCode))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        const auto& filename = entry.path().filename().string();
        int32_t pid = 0;

        // Check if directory name is a number (process ID)
        auto result = std::from_chars(filename.data(), filename.data() + filename.size(), pid);
        if (result.ec != std::errc{} || pid <= 0)
        {
            continue;
        }

        ProcessCounters counters{};
        if (!parseProcessStat(pid, counters))
        {
            spdlog::debug("Failed to parse {}", (procPath / std::to_string(pid) / "stat").string());
            continue;
        }

        parseProcessStatm(pid, counters);
        parseProcessStatus(pid, counters, m_ProcRoot);
        parseProcessCmdline(pid, counters, m_ProcRoot);
        // CPU affinity is always safe to query; failures zero the mask
        parseProcessAffinity(pid, counters);

        // Count open file descriptors (may fail for some processes due to permissions)
        countProcessFds(pid, counters, m_ProcRoot);

        // Only attempt I/O counters if we know they're readable
        // Use std::call_once for thread-safe lazy initialization; relaxed ordering is sufficient
        std::call_once(m_IoCountersCheckFlag,
                       [this]() { m_IoCountersAvailable.store(checkIoCountersAvailability(m_ProcRoot), std::memory_order_relaxed); });
        if (m_IoCountersAvailable.load(std::memory_order_relaxed))
        {
            parseProcessIo(pid, counters, m_ProcRoot);
        }
        counters.status = getProcessStatus(pid, m_ProcRoot); // Get cgroup freezer status
        processes.push_back(std::move(counters));
    }

    if (errorCode)
    {
        spdlog::warn("Error iterating {}: {}", procPath.string(), errorCode.message());
    }

    // Attribute energy to processes if power monitoring is available
    if (m_HasPowerCap)
    {
        attributeEnergyToProcesses(processes);
    }

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    // Attribute network bytes to processes if socket stats are available
    if (m_HasNetworkCounters && m_SocketStats)
    {
        attributeNetworkToProcesses(processes);
    }
#endif

    return processes;
}

ProcessCapabilities LinuxProcessProbe::capabilities() const
{
    // Check I/O counters availability on first call (thread-safe)
    std::call_once(m_IoCountersCheckFlag,
                   [this]() { m_IoCountersAvailable.store(checkIoCountersAvailability(m_ProcRoot), std::memory_order_release); });

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    const bool hasNetworkCounters = m_HasNetworkCounters;
#else
    const bool hasNetworkCounters = false;
#endif

    // Reduced privileges: FD counts (/proc/[pid]/fd) and I/O counters (/proc/[pid]/io)
    // for processes owned by other users are unavailable unless running as root.
    const bool reducedPrivileges = (geteuid() != 0);

    return ProcessCapabilities{.hasIoCounters = m_IoCountersAvailable.load(std::memory_order_acquire),
                               .hasThreadCount = true,
                               .hasHandleCount = true, // Can count FDs in /proc/[pid]/fd (own processes only when non-root)
                               .hasUserSystemTime = true,
                               .hasStartTime = true,
                               .hasUser = true,       // From /proc/[pid]/status Uid field
                               .hasCommand = true,    // From /proc/[pid]/cmdline
                               .hasNice = true,       // From /proc/[pid]/stat
                               .hasPageFaults = true, // From /proc/[pid]/stat (minflt + majflt)
                               .hasPeakRss = false,
                               .hasCpuAffinity = true,                     // From sched_getaffinity
                               .hasNetworkCounters = hasNetworkCounters,   // From Netlink INET_DIAG (if available)
                               .hasPowerUsage = m_HasPowerCap,             // Available if RAPL is detected
                               .hasStatus = true,                          // From cgroup freezer state
                               .hasReducedPrivileges = reducedPrivileges}; // Non-root: incomplete FD/IO data
}

uint64_t LinuxProcessProbe::totalCpuTime() const
{
    return readTotalCpuTime();
}

long LinuxProcessProbe::ticksPerSecond() const
{
    return m_TicksPerSecond;
}

bool LinuxProcessProbe::parseProcessStat(int32_t pid, ProcessCounters& counters) const
{
    // Format: /proc/[pid]/stat — single line
    // Fields: pid (comm) state ppid pgrp session tty_nr tpgid flags
    //         minflt cminflt majflt cmajflt utime stime cutime cstime
    //         priority nice num_threads itrealvalue starttime vsize rss ...

    const std::string statPath = (m_ProcRoot / std::to_string(pid) / "stat").string();

    // 1 KiB is ample: comm is kernel-capped at 15 chars, and the remaining
    // ~22 numeric fields are at most ~462 bytes total.
    std::array<char, 1024> buf{};
    const std::size_t len = readProcFile(statPath.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return false;
    }

    const char* const beg = buf.data();
    const char* const end = buf.data() + len;

    // Process name is in parentheses; find first '(' and last ')' to handle
    // names that themselves contain parentheses (e.g. "process (name)").
    const char* nameStart = beg;
    while (nameStart < end && *nameStart != '(')
    {
        ++nameStart;
    }
    if (nameStart >= end)
    {
        return false;
    }

    const char* nameEnd = end - 1;
    while (nameEnd > nameStart && *nameEnd != ')')
    {
        --nameEnd;
    }
    if (nameEnd <= nameStart)
    {
        return false;
    }

    counters.pid = pid;
    counters.name = std::string(nameStart + 1, static_cast<std::size_t>(nameEnd - nameStart - 1));

    // Fields follow the closing ')': ") state ppid pgrp ..."
    const char* q = nameEnd + 1;
    if (q < end && *q == ' ')
    {
        ++q;
    }

    char stateChar = '?';
    int32_t parentPid = 0;
    int32_t pgrp = 0;
    int32_t session = 0;
    int32_t ttyNr = 0;
    int32_t tpgid = 0;
    uint32_t flags = 0;
    uint64_t minflt = 0;
    uint64_t cminflt = 0;
    uint64_t majflt = 0;
    uint64_t cmajflt = 0;
    uint64_t utime = 0;
    uint64_t stime = 0;
    int64_t cutime = 0;
    int64_t cstime = 0;
    int64_t priority = 0;
    int64_t nice = 0;
    int64_t numThreads = 0;
    int64_t itrealvalue = 0;
    uint64_t starttime = 0;
    uint64_t vsize = 0;
    int64_t rss = 0;

    // State is a single character; skip leading whitespace then read it.
    q = skipSpaces(q, end);
    if (q >= end)
    {
        return false;
    }
    stateChar = *q++;

    // clang-format off
    if (!parseNum(q, end, parentPid)  || !parseNum(q, end, pgrp)        ||
        !parseNum(q, end, session)    || !parseNum(q, end, ttyNr)       ||
        !parseNum(q, end, tpgid)      || !parseNum(q, end, flags)       ||
        !parseNum(q, end, minflt)     || !parseNum(q, end, cminflt)     ||
        !parseNum(q, end, majflt)     || !parseNum(q, end, cmajflt)     ||
        !parseNum(q, end, utime)      || !parseNum(q, end, stime)       ||
        !parseNum(q, end, cutime)     || !parseNum(q, end, cstime)      ||
        !parseNum(q, end, priority)   || !parseNum(q, end, nice)        ||
        !parseNum(q, end, numThreads) || !parseNum(q, end, itrealvalue) ||
        !parseNum(q, end, starttime)  || !parseNum(q, end, vsize)       ||
        !parseNum(q, end, rss))
    // clang-format on
    {
        return false;
    }

    counters.state = stateChar;
    counters.parentPid = parentPid;
    counters.userTime = utime;
    counters.systemTime = stime;
    counters.threadCount = clampToI32((numThreads > 0) ? numThreads : 1);
    counters.startTimeTicks = starttime;

    // Convert start time from jiffies since boot to Unix epoch seconds
    // startTimeTicks is in clock ticks (jiffies), m_BootTimeEpoch is Unix epoch seconds
    if (m_BootTimeEpoch > 0 && m_TicksPerSecond > 0)
    {
        const auto secondsSinceBoot = starttime / static_cast<uint64_t>(m_TicksPerSecond);
        constexpr auto maxEpoch = std::numeric_limits<uint64_t>::max();

        // Overflow protection: ensure addition won't wrap
        if (secondsSinceBoot <= (maxEpoch - m_BootTimeEpoch))
        {
            counters.startTimeEpoch = m_BootTimeEpoch + secondsSinceBoot;
        }
        else
        {
            // On overflow, mark start time as unknown (0 is treated as invalid/unknown elsewhere)
            counters.startTimeEpoch = 0;
        }
    }

    counters.virtualBytes = vsize;
    counters.rssBytes = toU64PositiveOr(rss, 0ULL) * m_PageSize;
    counters.nice = clampToI32(nice);
    counters.pageFaultCount = minflt + majflt; // Total page faults (minor + major)

    return true;
}

void LinuxProcessProbe::parseProcessStatm(int32_t pid, ProcessCounters& counters) const
{
    // Format: /proc/[pid]/statm
    // Fields: size resident shared text lib data dt (all in pages)

    const std::string statmPath = (m_ProcRoot / std::to_string(pid) / "statm").string();
    std::array<char, 128> buf{};
    const std::size_t len = readProcFile(statmPath.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return;
    }

    const char* q = buf.data();
    const char* const end = buf.data() + len;
    uint64_t size = 0;
    uint64_t resident = 0;
    uint64_t shared = 0;
    if (parseNum(q, end, size) && parseNum(q, end, resident) && parseNum(q, end, shared))
    {
        // statm gives more accurate RSS, update if available
        counters.rssBytes = resident * m_PageSize;
        counters.sharedBytes = shared * m_PageSize;
    }
}

void LinuxProcessProbe::parseProcessStatus(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot)
{
    // Read /proc/[pid]/status for UID (owner) information
    // Format is key:value pairs, one per line
    // We need: Uid: <real> <effective> <saved> <filesystem>

    const std::string statusPath = (procRoot / std::to_string(pid) / "status").string();
    constexpr std::size_t BUF_SIZE = 2048;
    std::array<char, BUF_SIZE> buf{};
    const std::size_t len = readProcFile(statusPath.c_str(), buf.data(), BUF_SIZE);
    if (len == 0)
    {
        return;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;
    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        // Look for "Uid:" line
        constexpr std::string_view UID_PREFIX = "Uid:";
        if (static_cast<std::size_t>(lineEnd - p) > UID_PREFIX.size() && std::string_view(p, UID_PREFIX.size()) == UID_PREFIX)
        {
            // Skip "Uid:" and whitespace, parse first UID with from_chars (no alloc)
            const char* ptr = p + UID_PREFIX.size();
            while (ptr < lineEnd && (*ptr == ' ' || *ptr == '\t'))
            {
                ++ptr;
            }
            uid_t realUid = 0;
            if (std::from_chars(ptr, lineEnd, realUid).ec == std::errc{})
            {
                counters.user = getUsername(realUid);
            }
            break;
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }
}

void LinuxProcessProbe::parseProcessCmdline(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/[pid]/cmdline
    // Arguments are separated by NUL bytes

    const std::string cmdlinePath = (procRoot / std::to_string(pid) / "cmdline").string();

    // Read until EOF so long command lines are not truncated.
    std::vector<char> buf = readProcFileFull(cmdlinePath.c_str());
    const std::size_t len = buf.size();

    if (len == 0)
    {
        // Distinguish "file unreadable" (e.g. hidepid, permission denied) from
        // "file readable but empty" (kernel threads have an empty cmdline).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
        const int fd = ::open(cmdlinePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1)
        {
            // Cannot read the file — leave command unchanged rather than
            // incorrectly labelling a non-kernel process as a kernel thread.
            return;
        }
        ::close(fd);
        // File opened but is empty: genuine kernel thread — use bracketed name.
        counters.command = "[" + counters.name + "]";
        return;
    }

    // Replace NUL argument separators with spaces, then trim trailing space.
    std::string cmdline(buf.data(), len);
    for (auto& c : cmdline)
    {
        if (c == '\0')
        {
            c = ' ';
        }
    }
    while (!cmdline.empty() && cmdline.back() == ' ')
    {
        cmdline.pop_back();
    }

    counters.command = std::move(cmdline);
}

void LinuxProcessProbe::parseProcessAffinity(int32_t pid, ProcessCounters& counters)
{
    // Use sched_getaffinity to read CPU affinity mask for the process
    // This returns which CPU cores the process is allowed to run on

    // NOLINTNEXTLINE(misc-include-cleaner) - cpu_set_t is provided by <sched.h>
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);

    // sched_getaffinity returns the affinity for the main thread of the process
    if (sched_getaffinity(pid, sizeof(cpu_set_t), &cpuSet) == 0)
    {
        // Convert cpu_set_t to a bitmask that fits in uint64_t
        // This limits us to 64 cores, which is reasonable for most systems
        const int maxCpus = std::min(CPU_SETSIZE, 64);
        std::uint64_t mask = 0;
        for (int cpu = 0; cpu < maxCpus; ++cpu)
        {
            if (CPU_ISSET(static_cast<size_t>(cpu), &cpuSet) != 0)
            {
                mask |= (1ULL << cpu);
            }
        }
        counters.cpuAffinityMask = mask;
    }
    else
    {
        // If sched_getaffinity fails (e.g., permission denied), set mask to 0
        counters.cpuAffinityMask = 0;
    }
}

void LinuxProcessProbe::parseProcessIo(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/[pid]/io
    // Key-value pairs, one per line:
    // rchar: <bytes>
    // wchar: <bytes>
    // syscr: <count>
    // syscw: <count>
    // read_bytes: <bytes>  <- actual I/O from storage layer
    // write_bytes: <bytes> <- actual I/O to storage layer
    // cancelled_write_bytes: <bytes>
    //
    // Note: This file requires CAP_DAC_READ_SEARCH capability or running as root,
    // or being the owner of the process. If we can't read it, we silently skip
    // (capabilities() already reports hasIoCounters = false by default).

    const std::string ioPath = (procRoot / std::to_string(pid) / "io").string();
    constexpr std::size_t BUF_SIZE = 512;
    std::array<char, BUF_SIZE> buf{};
    const std::size_t len = readProcFile(ioPath.c_str(), buf.data(), BUF_SIZE);
    if (len == 0)
    {
        // Common case: insufficient permissions, just return
        return;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;
    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        constexpr std::string_view readPrefix = "read_bytes:";
        constexpr std::string_view writePrefix = "write_bytes:";

        const std::string_view lineView(p, static_cast<std::size_t>(lineEnd - p));

        if (lineView.starts_with(readPrefix))
        {
            // Parse value with from_chars: no substr alloc, no istringstream alloc
            const char* ptr = p + readPrefix.size();
            while (ptr < lineEnd && (*ptr == ' ' || *ptr == '\t'))
            {
                ++ptr;
            }
            uint64_t readBytes = 0;
            if (std::from_chars(ptr, lineEnd, readBytes).ec == std::errc{})
            {
                counters.readBytes = readBytes;
            }
        }
        else if (lineView.starts_with(writePrefix))
        {
            const char* ptr = p + writePrefix.size();
            while (ptr < lineEnd && (*ptr == ' ' || *ptr == '\t'))
            {
                ++ptr;
            }
            uint64_t writeBytes = 0;
            if (std::from_chars(ptr, lineEnd, writeBytes).ec == std::errc{})
            {
                counters.writeBytes = writeBytes;
            }
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }
}

void LinuxProcessProbe::countProcessFds(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot)
{
    // Count entries in /proc/[pid]/fd directory.
    // Each entry is a symlink to an open file descriptor.
    // Note: May fail due to permissions (needs same user or root).

    const auto fdPath = procRoot / std::to_string(pid) / "fd";

    int32_t count = 0;
    try
    {
        // Don't use error_code variant because errors during iteration
        // (not just construction) won't be captured in it. Rely on exceptions.
        for (const auto& entry : std::filesystem::directory_iterator(fdPath))
        {
            (void) entry; // We just count entries
            ++count;
        }
        // Only set if we successfully enumerated the directory
        counters.handleCount = count;
    }
    catch (const std::exception& ex)
    {
        // Permission errors and other exceptional situations - leave handleCount at 0
        spdlog::debug("LinuxProcessProbe: failed to enumerate FDs for pid {} at {}: {}", pid, fdPath.string(), ex.what());
    }
}

bool LinuxProcessProbe::checkIoCountersAvailability(const std::filesystem::path& procRoot)
{
    // Check if procRoot/self/io is readable to determine I/O counter availability.
    // This file requires CAP_DAC_READ_SEARCH capability or root privileges,
    // or being the owner of the target process.
    const std::string selfIoPath = (procRoot / "self" / "io").string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(selfIoPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return false;
    }
    ::close(fd);
    return true;
}

std::string LinuxProcessProbe::getProcessStatus(int32_t pid, const std::filesystem::path& procRoot)
{
    // Try cgroup v2 first: freezer.state
    const auto cgroupV2FreezerPath = std::format("/sys/fs/cgroup/{}/freezer.state", pid);
    {
        std::array<char, 16> stateBuf{};
        const std::size_t stateLen = readProcFile(cgroupV2FreezerPath.c_str(), stateBuf.data(), stateBuf.size());
        if (stateLen > 0)
        {
            const std::string_view state(stateBuf.data(), stateLen);
            if (state.starts_with("FROZEN") || state.starts_with("FREEZING"))
            {
                return "Suspended";
            }
        }
    }

    // Fallback to cgroup v1 freezer hierarchy
    // /proc/[pid]/cgroup lists all cgroups for the process
    const auto cgroupPath = (procRoot / std::to_string(pid) / "cgroup").string();
    constexpr std::size_t CGROUP_BUF = 2048;
    std::array<char, CGROUP_BUF> cgroupBuf{};
    const std::size_t cgroupLen = readProcFile(cgroupPath.c_str(), cgroupBuf.data(), CGROUP_BUF);
    if (cgroupLen > 0)
    {
        const char* p = cgroupBuf.data();
        const char* const end = cgroupBuf.data() + cgroupLen;
        while (p < end)
        {
            const char* lineEnd = p;
            while (lineEnd < end && *lineEnd != '\n')
            {
                ++lineEnd;
            }

            // Format: hierarchy-ID:controllers:cgroup-path
            const char* firstColon = p;
            while (firstColon < lineEnd && *firstColon != ':')
            {
                ++firstColon;
            }
            const char* secondColon = (firstColon < lineEnd) ? firstColon + 1 : lineEnd;
            while (secondColon < lineEnd && *secondColon != ':')
            {
                ++secondColon;
            }

            if (firstColon < lineEnd && secondColon < lineEnd)
            {
                const std::string_view controllers(firstColon + 1, static_cast<std::size_t>(secondColon - firstColon - 1));
                const std::string_view cgroupSubPath(secondColon + 1, static_cast<std::size_t>(lineEnd - secondColon - 1));

                // Check if this line has the freezer controller
                if (controllers.contains("freezer"))
                {
                    // Build path: /sys/fs/cgroup/freezer/<cgroup-path>/freezer.state
                    // Skip if cgroupSubPath is empty or doesn't start with /
                    if (!cgroupSubPath.empty() && cgroupSubPath[0] == '/')
                    {
                        const std::filesystem::path freezePathV1 =
                            std::filesystem::path("/sys/fs/cgroup/freezer") / cgroupSubPath.substr(1) / "freezer.state";
                        const std::string freezePathStr = freezePathV1.string();
                        std::array<char, 16> freezeStateBuf{};
                        const std::size_t freezeLen = readProcFile(freezePathStr.c_str(), freezeStateBuf.data(), freezeStateBuf.size());
                        if (freezeLen > 0)
                        {
                            const std::string_view state(freezeStateBuf.data(), freezeLen);
                            if (state.starts_with("FROZEN") || state.starts_with("FREEZING"))
                            {
                                return "Suspended";
                            }
                        }
                    }
                }
            }

            p = (lineEnd < end) ? lineEnd + 1 : end;
        }
    }

    // No special status
    return {};
}
uint64_t LinuxProcessProbe::readTotalCpuTime() const
{
    // Format: /proc/stat — first line: "cpu user nice system idle iowait irq softirq steal …"
    // We only need the first line, so 256 bytes is ample.

    const std::string statPath = (m_ProcRoot / "stat").string();
    std::array<char, 256> buf{};
    const std::size_t len = readProcFile(statPath.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        spdlog::warn("Failed to open {}", statPath);
        return 0;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;

    // First line must start with "cpu "
    if (len < 4 || p[0] != 'c' || p[1] != 'p' || p[2] != 'u' || p[3] != ' ')
    {
        spdlog::warn("Failed to parse {}", statPath);
        return 0;
    }
    p += 4; // skip "cpu "

    // Find end of first line
    const char* lineEnd = p;
    while (lineEnd < end && *lineEnd != '\n')
    {
        ++lineEnd;
    }

    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;

    if (!parseNum(p, lineEnd, user) || !parseNum(p, lineEnd, nice) || !parseNum(p, lineEnd, system) || !parseNum(p, lineEnd, idle) ||
        !parseNum(p, lineEnd, iowait) || !parseNum(p, lineEnd, irq) || !parseNum(p, lineEnd, softirq) || !parseNum(p, lineEnd, steal))
    {
        spdlog::warn("Failed to parse {}", statPath);
        return 0;
    }

    // Total CPU time = all fields combined
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

uint64_t LinuxProcessProbe::readBootTime(const std::filesystem::path& procRoot)
{
    // Format: /proc/stat contains a line: btime <epoch_seconds>
    // btime is the time the system booted in seconds since Unix epoch
    // The btime line appears after all cpu lines; on systems with very large core
    // counts /proc/stat can exceed 64 KiB, so read until EOF.

    const std::string statPath = (procRoot / "stat").string();
    const std::vector<char> buf = readProcFileFull(statPath.c_str());
    if (buf.empty())
    {
        spdlog::warn("Failed to open {} for boot time", statPath);
        return 0;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + buf.size();
    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        constexpr std::string_view BTIME_PREFIX = "btime ";
        if (static_cast<std::size_t>(lineEnd - p) > BTIME_PREFIX.size() && std::string_view(p, BTIME_PREFIX.size()) == BTIME_PREFIX)
        {
            uint64_t bootTime = 0;
            const char* begin = p + BTIME_PREFIX.size();
            if (std::from_chars(begin, lineEnd, bootTime).ec == std::errc{})
            {
                return bootTime;
            }
            break;
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }

    return 0;
}

uint64_t LinuxProcessProbe::systemTotalMemory() const
{
    const std::string meminfoPath = (m_ProcRoot / "meminfo").string();
    constexpr std::size_t BUF_SIZE = 4096;
    std::array<char, BUF_SIZE> buf{};
    const std::size_t len = readProcFile(meminfoPath.c_str(), buf.data(), BUF_SIZE);
    if (len == 0)
    {
        spdlog::error("Failed to open {}", meminfoPath);
        return 0;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;
    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        constexpr std::string_view MEM_TOTAL_PREFIX = "MemTotal:";
        if (static_cast<std::size_t>(lineEnd - p) > MEM_TOTAL_PREFIX.size() &&
            std::string_view(p, MEM_TOTAL_PREFIX.size()) == MEM_TOTAL_PREFIX)
        {
            const char* begin = p + MEM_TOTAL_PREFIX.size();
            while (begin < lineEnd && (*begin == ' ' || *begin == '\t'))
            {
                ++begin;
            }
            uint64_t kb = 0;
            const auto [ptr, ec] = std::from_chars(begin, lineEnd, kb);
            if (ec == std::errc{})
            {
                (void) ptr;
                return kb * 1024ULL;
            }
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }

    spdlog::warn("MemTotal not found in /proc/meminfo");
    return 0;
}

bool LinuxProcessProbe::detectPowerCap()
{
    // Try to find Intel RAPL package energy file
    // Common paths: /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj
    const std::vector<std::string> possiblePaths = {
        "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
        "/sys/class/powercap/intel-rapl:0/energy_uj",
    };

    for (const auto& path : possiblePaths)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd != -1)
        {
            ::close(fd);
            m_PowerCapPath = path;
            return true;
        }
    }

    // Try to enumerate powercap directory
    std::error_code ec;
    const std::filesystem::path powercapDir("/sys/class/powercap");
    if (std::filesystem::exists(powercapDir, ec) && std::filesystem::is_directory(powercapDir, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(powercapDir, ec))
        {
            if (entry.is_directory() && entry.path().filename().string().starts_with("intel-rapl"))
            {
                std::filesystem::path energyFile = entry.path() / "energy_uj";
                if (std::filesystem::exists(energyFile, ec))
                {
                    m_PowerCapPath = energyFile.string();
                    return true;
                }

                // Try package:0 subdirectory
                const std::filesystem::path packageDir = entry.path() / "intel-rapl:0";
                energyFile = packageDir / "energy_uj";
                if (std::filesystem::exists(energyFile, ec))
                {
                    m_PowerCapPath = energyFile.string();
                    return true;
                }
            }
        }
    }

    return false;
}

uint64_t LinuxProcessProbe::readSystemEnergy() const
{
    if (m_PowerCapPath.empty())
    {
        return 0;
    }

    std::array<char, 32> buf{};
    const std::size_t len = readProcFile(m_PowerCapPath.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return 0;
    }

    const char* p = buf.data();
    uint64_t energyUj = 0;
    if (!parseNum(p, buf.data() + len, energyUj))
    {
        return 0;
    }

    return energyUj; // Already in microjoules
}

void LinuxProcessProbe::attributeEnergyToProcesses(std::vector<ProcessCounters>& processes) const
{
    // Read system-wide energy
    const uint64_t systemEnergy = readSystemEnergy();
    if (systemEnergy == 0)
    {
        return;
    }

    // Calculate total CPU time across all processes
    uint64_t totalProcessCpuTime = 0;
    for (const auto& proc : processes)
    {
        totalProcessCpuTime += (proc.userTime + proc.systemTime);
    }

    // Avoid division by zero
    if (totalProcessCpuTime == 0)
    {
        return;
    }

    // Attribute energy proportionally based on CPU usage
    // This is an approximation: energy per process = systemEnergy * (processCpuTime / totalCpuTime)
    for (auto& proc : processes)
    {
        const uint64_t processCpuTime = proc.userTime + proc.systemTime;
        const double cpuProportion = static_cast<double>(processCpuTime) / static_cast<double>(totalProcessCpuTime);
        proc.energyMicrojoules = static_cast<uint64_t>(static_cast<double>(systemEnergy) * cpuProportion);
    }
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
void LinuxProcessProbe::attributeNetworkToProcesses(std::vector<ProcessCounters>& processes) const
{
    if (!m_SocketStats)
    {
        return;
    }

    // Query all TCP/UDP sockets with their byte counters
    const std::vector<SocketStats> sockets = m_SocketStats->queryAllSockets();
    if (sockets.empty())
    {
        return;
    }

    // Refresh inode-to-PID map on a TTL basis to avoid scanning /proc/[pid]/fd/* every
    // enumerate(). The rebuild slot is claimed by advancing m_InodeToPidCacheTime under
    // the initial lock, so only one thread rebuilds per TTL window while all others
    // continue using the previous shared_ptr snapshot (see #460).
    std::shared_ptr<const std::unordered_map<std::uint64_t, std::int32_t>> inodeToPidPtr;
    bool needsRebuild = false;
    {
        const std::scoped_lock lock{m_InodePidCacheMutex};
        const auto now = std::chrono::steady_clock::now();
        const auto cacheAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_InodeToPidCacheTime).count();
        needsRebuild = (cacheAgeMs >= Domain::Sampling::INODE_PID_CACHE_TTL_MS);
        if (needsRebuild)
        {
            // Claim the rebuild slot: advance the timestamp now so any other thread that
            // checks while we are scanning /proc sees a fresh time and skips rebuilding.
            m_InodeToPidCacheTime = now;
        }
        inodeToPidPtr = m_InodeToPidCache; // snapshot current (possibly stale) pointer
    }
    if (needsRebuild)
    {
        // Build the map outside the lock; concurrent threads keep using the old snapshot.
        auto rebuilt = std::make_shared<const std::unordered_map<std::uint64_t, std::int32_t>>(buildInodeToPidMap());
        {
            const std::scoped_lock lock{m_InodePidCacheMutex};
            if (!rebuilt->empty())
            {
                m_InodeToPidCache = std::move(rebuilt);
                m_InodeToPidCacheTime = std::chrono::steady_clock::now();
                inodeToPidPtr = m_InodeToPidCache;
            }
            else
            {
                // Preserve the previous snapshot when procfs enumeration transiently
                // produces no entries; allow a quick retry instead of waiting the full TTL.
                constexpr auto EMPTY_REBUILD_RETRY_MS = std::chrono::milliseconds{100};
                const auto ttl = std::chrono::milliseconds{Domain::Sampling::INODE_PID_CACHE_TTL_MS};
                const auto retryDelay = std::min(EMPTY_REBUILD_RETRY_MS, ttl);
                m_InodeToPidCacheTime = std::chrono::steady_clock::now() - (ttl - retryDelay);
                // inodeToPidPtr already holds the previous (possibly non-empty) snapshot
            }
        }
    }
    if (!inodeToPidPtr || inodeToPidPtr->empty())
    {
        return;
    }

    // Aggregate socket bytes by PID
    const auto& inodeToPid = *inodeToPidPtr;
    const auto pidStats = aggregateByPid(sockets, inodeToPid);

    // Apply network stats to processes
    for (auto& proc : processes)
    {
        auto it = pidStats.find(proc.pid);
        if (it != pidStats.end())
        {
            const auto& [received, sent] = it->second;
            proc.netReceivedBytes = received;
            proc.netSentBytes = sent;
        }
    }
}
#endif // TASKSMACK_HAS_NETLINK_SOCKET_STATS

} // namespace Platform

#endif
