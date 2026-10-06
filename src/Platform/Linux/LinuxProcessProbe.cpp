// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<pwd.h>) && __has_include(<unistd.h>)

#include "LinuxProcessProbe.h"

#include "CgroupFreezeStatus.h"
#include "Domain/SamplingConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessProbe.h"
#include "Platform/PlatformConfig.h"
#include "UserNameLookup.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "NetlinkSocketStats.h"
#endif

#include "Platform/ProcessTypes.h"
#include "ProcParsing.h"
#include "ProcPrivileges.h"
#include "ProcessName.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
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

using ProcParsing::FdGuard;
using ProcParsing::parseNum;
using ProcParsing::readProcFile;
using ProcParsing::readProcFileFull;
using ProcParsing::skipSpaces;

/// Read a whole /proc file of up to maxBytes, for the rare one that overflows a caller's stack buffer.
/// Each try re-reads from the start into a buffer twice the last one's size, so the text comes from a
/// single read. Returns the bytes read -- a file longer than maxBytes comes back cut off at maxBytes
/// -- or an empty vector on failure.
[[nodiscard]] std::vector<char> readProcFileBounded(const std::string& path, std::size_t initialSize, std::size_t maxBytes)
{
    std::vector<char> buf;
    for (std::size_t size = std::min(initialSize, maxBytes);; size = std::min(size * 2, maxBytes))
    {
        buf.resize(size);
        const std::size_t len = readProcFile(path.c_str(), buf.data(), buf.size());
        if (len < size || size == maxBytes)
        {
            buf.resize(len);
            return buf;
        }
    }
}

/// Whether text has a line starting with prefix that ends in a newline (not cut off by a short read).
[[nodiscard]] bool hasCompleteLine(std::string_view text, std::string_view prefix) noexcept
{
    for (std::size_t pos = text.find(prefix); pos != std::string_view::npos; pos = text.find(prefix, pos + 1))
    {
        if (pos == 0 || text[pos - 1] == '\n')
        {
            return text.find('\n', pos) != std::string_view::npos;
        }
    }
    return false;
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

    // Look up username from passwd database (thread-safe version); fall back to the UID as a string.
    std::string username = lookUpUserName(uid, ::getpwuid_r).value_or(std::to_string(uid));

    cache[uid] = username;
    return username;
}

} // namespace

LinuxProcessProbe::LinuxProcessProbe() : LinuxProcessProbe(std::filesystem::path("/proc"))
{}

LinuxProcessProbe::LinuxProcessProbe(std::filesystem::path procRoot)
    : LinuxProcessProbe(std::move(procRoot), std::filesystem::path("/sys/class/powercap"))
{}

LinuxProcessProbe::LinuxProcessProbe(std::filesystem::path procRoot, std::filesystem::path powercapRoot)
    : m_ProcRoot(std::move(procRoot)),
      m_PowercapRoot(std::move(powercapRoot)),
      m_TicksPerSecond(sysconf(_SC_CLK_TCK)),
      m_PageSize(toU64PositiveOr(sysconf(_SC_PAGESIZE), 4096ULL)),
      m_BootTimeEpoch(readBootTime(m_ProcRoot))
{
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    m_InodeMapEarlyRebuildInterval = std::chrono::milliseconds{Domain::Sampling::INODE_PID_CACHE_EARLY_REBUILD_MS};
#endif
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
    auto initialSocketStats = std::make_shared<NetlinkSocketStats>();
    m_HasNetworkCounters = initialSocketStats->isAvailable();
    m_SocketStats = std::move(initialSocketStats); // no lock needed: not yet shared across threads
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
    // Construct the replacement before taking the lock (socket creation, not the shared
    // state, is the expensive part); only the swap itself needs to be guarded.
    auto newSocketStats = std::make_shared<NetlinkSocketStats>(ttlMs);

    const std::scoped_lock lock(m_SocketStatsMutex);
    if (m_SocketStats)
    {
        // Swap under the lock: a concurrent enumerate() on the sampler thread either sees
        // the old or the new NetlinkSocketStats in full via socketStats(), and the old one
        // stays alive (via shared_ptr refcounting) until any in-flight call using it
        // returns, instead of being destroyed out from under it.
        m_SocketStats = std::move(newSocketStats);
    }
}

void LinuxProcessProbe::setSocketStatsForTesting(std::shared_ptr<NetlinkSocketStats> socketStats)
{
    m_HasNetworkCounters = socketStats != nullptr && socketStats->isAvailable();
    const std::scoped_lock lock(m_SocketStatsMutex);
    m_SocketStats = std::move(socketStats);
}

std::shared_ptr<NetlinkSocketStats> LinuxProcessProbe::socketStats() const
{
    const std::scoped_lock lock(m_SocketStatsMutex);
    return m_SocketStats;
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
        parseProcessStatus(pid, counters, m_ProcRoot); // Owner and CPU affinity
        parseProcessCmdline(pid, counters, m_ProcRoot);

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
        else
        {
            counters.ioCountersAvailable = false; // not read at all (capabilities() reports hasIoCounters = false)
        }
        counters.status = getProcessStatus(pid, m_ProcRoot); // Get cgroup freezer status
        processes.push_back(std::move(counters));
    }

    if (errorCode)
    {
        spdlog::warn("Error iterating {}: {}", procPath.string(), errorCode.message());
    }

    // The system total that the processes' CPU deltas are divided by, taken now -- right after
    // their stat reads, so nothing that runs before totalCpuTime() is called can skew its interval
    // from theirs (#1119).
    m_TotalCpuTimeAtEnumerate.store(readTotalCpuTime(), std::memory_order_relaxed);

    if (m_EnumerateTailHook)
    {
        m_EnumerateTailHook(); // Tests: time passes between the capture and totalCpuTime()
    }

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

    // Reduced privileges: FD counts (/proc/[pid]/fd), I/O counters (/proc/[pid]/io) and network
    // attribution for processes owned by other users need CAP_DAC_READ_SEARCH and CAP_SYS_PTRACE in
    // the effective set -- root with its normal capabilities has them, but root alone isn't enough
    // where capabilities are dropped (docs/guide/faq.md's setcap line).
    const std::vector<char> selfStatus = readProcFileFull((m_ProcRoot / "self" / "status").c_str());
    const bool reducedPrivileges = ProcPrivileges::hasReducedPrivileges(
        geteuid() == 0,
        ProcPrivileges::parseCapEff(selfStatus.empty() ? std::string_view{} : std::string_view(selfStatus.data(), selfStatus.size())));

    return ProcessCapabilities{.hasIoCounters = m_IoCountersAvailable.load(std::memory_order_acquire),
                               .hasThreadCount = true,
                               .hasHandleCount = true, // Can count FDs in /proc/[pid]/fd (others' need CAP_DAC_READ_SEARCH)
                               .hasUserSystemTime = true,
                               .hasStartTime = true,
                               .hasUser = true,       // From /proc/[pid]/status Uid field
                               .hasCommand = true,    // From /proc/[pid]/cmdline
                               .hasNice = true,       // From /proc/[pid]/stat
                               .hasPageFaults = true, // From /proc/[pid]/stat (minflt + majflt)
                               .hasPeakRss = false,
                               .hasCpuAffinity = true,                    // From status Cpus_allowed_list
                               .hasNetworkCounters = hasNetworkCounters,  // From Netlink INET_DIAG (if available)
                               .hasUdpNetworkCounters = false,            // sock_diag has no UDP byte counters (#1101)
                               .hasPowerUsage = m_HasPowerCap,            // Available if RAPL is detected
                               .hasStatus = true,                         // From cgroup freezer state
                               .hasReducedPrivileges = reducedPrivileges, // Incomplete FD/IO/network data
                               .hasSharedMemory = true};                  // From /proc/[pid]/statm
}

uint64_t LinuxProcessProbe::totalCpuTime() const
{
    // The value enumerate() captured after its stat pass, once -- even a failed read's 0; otherwise a
    // fresh read.
    if (const uint64_t captured = m_TotalCpuTimeAtEnumerate.exchange(NO_CAPTURED_TOTAL, std::memory_order_relaxed);
        captured != NO_CAPTURED_TOTAL)
    {
        return captured;
    }
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
    // Read /proc/[pid]/status for the owner and the CPU affinity.
    // Format is key:value pairs, one per line. We need:
    //   Uid: <real> <effective> <saved> <filesystem>
    //   Cpus_allowed_list: <kernel CPU list, e.g. 0-3,64-127>
    // The affinity used to come from sched_getaffinity(), packed into a 64-bit mask that dropped every
    // CPU from 64 up (#1247). The list here has no width limit, is already in the file this reads,
    // and follows procRoot (sched_getaffinity() asked the real kernel about a test's fake pid).

    const std::string statusPath = (procRoot / std::to_string(pid) / "status").string();
    // Big enough for the usual file, Cpus_allowed's hex mask on a many-CPU kernel included, without allocating.
    constexpr std::size_t BUF_SIZE = 8192;
    std::array<char, BUF_SIZE> buf{};
    const std::size_t len = readProcFile(statusPath.c_str(), buf.data(), BUF_SIZE);
    if (len == 0)
    {
        return;
    }
    constexpr std::string_view UID_PREFIX = "Uid:";
    constexpr std::string_view CPUS_ALLOWED_LIST_PREFIX = "Cpus_allowed_list:";
    std::string_view text(buf.data(), len);
    std::vector<char> fullStatus;
    if (len == BUF_SIZE && !hasCompleteLine(text, CPUS_ALLOWED_LIST_PREFIX))
    {
        // readProcFile() truncates silently at BUF_SIZE: a sparse Cpus_allowed_list (0,2,...,8190 is
        // about 20 KiB) or a long Groups: line before it runs past the buffer. Read the whole file
        // instead, bounded well above any real status file; if that fails (the process just exited),
        // fall back to the prefix. Either way a Cpus_allowed_list line cut off by the end of what was
        // read is ignored below rather than read in part.
        constexpr std::size_t MAX_STATUS_SIZE = std::size_t{1} << 20U; // 1 MiB
        fullStatus = readProcFileBounded(statusPath, 4 * BUF_SIZE, MAX_STATUS_SIZE);
        if (!fullStatus.empty())
        {
            text = std::string_view(fullStatus.data(), fullStatus.size());
        }
        else
        {
            spdlog::debug(
                "LinuxProcessProbe: /proc/{}/status truncated at {} bytes; Uid:/Cpus_allowed_list: may have been missed", pid, BUF_SIZE);
        }
    }

    bool haveUid = false;
    bool haveAffinity = false;
    const char* p = text.data();
    const char* const end = text.data() + text.size();
    while (p < end && !(haveUid && haveAffinity))
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }
        const std::string_view line(p, static_cast<std::size_t>(lineEnd - p));

        if (!haveUid && line.size() > UID_PREFIX.size() && line.starts_with(UID_PREFIX))
        {
            haveUid = true;
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
        }
        else if (!haveAffinity && line.starts_with(CPUS_ALLOWED_LIST_PREFIX))
        {
            haveAffinity = true;
            // Only a whole line: one the buffer cut off could read "0-12" for "0-127".
            if (lineEnd < end)
            {
                if (auto affinity = CpuAffinity::fromCpuList(line.substr(CPUS_ALLOWED_LIST_PREFIX.size())))
                {
                    counters.cpuAffinity = std::move(*affinity);
                }
            }
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }
}

void LinuxProcessProbe::parseProcessCmdline(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/[pid]/cmdline
    // Arguments are separated by NUL bytes

    // A zombie has no command line left, and /proc/<pid>/cmdline may also be unreadable for another
    // user's process even though stat showed state Z. Either way it must not get the kernel-thread
    // label: mark it <defunct>, as ps does (#1155).
    if (counters.state == 'Z')
    {
        counters.command = counters.name + " <defunct>";
        return;
    }

    const std::string cmdlinePath = (procRoot / std::to_string(pid) / "cmdline").string();

    // Open once: distinguishes "unreadable" (permission denied, hidepid) from
    // "readable but empty" (kernel threads), avoiding a second open() call.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(cmdlinePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        // Cannot read the file — leave command unchanged rather than
        // incorrectly labelling a non-kernel process as a kernel thread.
        return;
    }

    const FdGuard guard{fd}; // ensures fd is closed on all paths, including exception paths
                             // (buf.reserve / buf.insert can throw on OOM)

    // Read until EOF so long command lines are not truncated.
    std::vector<char> buf;
    buf.reserve(4096);
    std::array<char, 4096> chunk{};
    bool readError = false;
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
            readError = true;
            break; // I/O error
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + static_cast<std::size_t>(n));
    }

    if (readError)
    {
        // Treat a read error the same as open() failure: leave command unchanged
        // rather than incorrectly labelling the process as a kernel thread.
        return;
    }

    if (buf.empty())
    {
        // File opened and fully read but empty: a kernel thread (zombies were handled above).
        counters.command = "[" + counters.name + "]";
        return;
    }

    // The kernel caps the name in /proc/[pid]/stat at 15 characters. Where it has been cut, the
    // full name is usually recoverable from the arguments, which are still NUL-separated at this
    // point -- so this costs no extra read. See ProcessName.h for when the result is trusted (#951).
    if (const std::string_view fullName = ProcessName::resolveFullName(counters.name, std::string_view(buf.data(), buf.size()));
        fullName.size() != counters.name.size())
    {
        counters.name = std::string(fullName);
    }

    // Replace NUL argument separators with spaces, then trim trailing space.
    std::string cmdline(buf.data(), buf.size());
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
    // Note: for another user's process this file needs CAP_DAC_READ_SEARCH (to open the owner-only
    // file) plus CAP_SYS_PTRACE (the read checks PTRACE_MODE_READ_FSCREDS) in the effective set --
    // root with its normal capabilities has them, but root alone isn't enough where capabilities are
    // dropped. If we can't read it -- typically another user's process without those capabilities --
    // the counters are marked unavailable rather than left at a 0 that reads as "no I/O" (#1110).

    const std::string ioPath = (procRoot / std::to_string(pid) / "io").string();
    constexpr std::size_t BUF_SIZE = 512;
    std::array<char, BUF_SIZE> buf{};
    const std::size_t len = readProcFile(ioPath.c_str(), buf.data(), BUF_SIZE);
    if (len == 0)
    {
        // Common case: insufficient permissions
        counters.ioCountersAvailable = false;
        return;
    }

    bool hasRead = false;
    bool hasWrite = false;
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
                hasRead = true;
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
                hasWrite = true;
            }
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }

    counters.ioCountersAvailable = hasRead && hasWrite;
}

void LinuxProcessProbe::countProcessFds(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot)
{
    // Count entries in /proc/[pid]/fd directory.
    // Each entry is a symlink to an open file descriptor.
    // Note: May fail due to permissions (needs same user or root).

    const auto fdPath = procRoot / std::to_string(pid) / "fd";

    int32_t count = 0;
    // Whether the fd links can be read, judged on the first entry that answers. Listing the directory
    // needs only DAC permission (CAP_DAC_READ_SEARCH for another user's process), but reading its
    // links -- which the socket inode-to-PID map does -- also needs ptrace access (CAP_SYS_PTRACE). A
    // process whose links can't be read has none of its connections attributed to it, so its network
    // counters are unknown, not "no traffic" (#1328).
    enum class LinkAccess : std::uint8_t
    {
        Unknown,
        Readable,
        Denied
    };
    LinkAccess linkAccess = LinkAccess::Unknown;
    try
    {
        // Don't use error_code variant because errors during iteration
        // (not just construction) won't be captured in it. Rely on exceptions.
        std::array<char, 64> linkTarget{};
        for (const auto& entry : std::filesystem::directory_iterator(fdPath))
        {
            ++count;
            if (linkAccess != LinkAccess::Unknown)
            {
                continue;
            }
            if (::readlink(entry.path().c_str(), linkTarget.data(), linkTarget.size()) >= 0 || errno == EINVAL)
            {
                linkAccess = LinkAccess::Readable; // EINVAL: not a link (a synthetic /proc), but access was granted
            }
            else if (errno == EACCES || errno == EPERM)
            {
                linkAccess = LinkAccess::Denied;
            }
            // Anything else (ENOENT: the fd closed since the listing): try the next entry.
        }
        // Only set if we successfully enumerated the directory
        counters.handleCount = count;
        if (linkAccess == LinkAccess::Denied)
        {
            counters.networkCountersAvailable = false;
        }
    }
    catch (const std::exception& ex)
    {
        // Permission errors (another user's process, without CAP_DAC_READ_SEARCH) and other exceptional situations:
        // the count is unknown, not 0 (#1110). The process's connections can't be attributed to it
        // either -- the socket inode-to-PID map is built from these same fd directories -- so its
        // network counters are unknown too, not "no traffic".
        counters.handleCount = 0;
        counters.handleCountAvailable = false;
        counters.networkCountersAvailable = false;
        spdlog::debug("LinuxProcessProbe: failed to enumerate FDs for pid {} at {}: {}", pid, fdPath.string(), ex.what());
    }
}

bool LinuxProcessProbe::checkIoCountersAvailability(const std::filesystem::path& procRoot)
{
    // Check if procRoot/self/io is readable to determine I/O counter availability.
    // Our own io file is always readable unless procfs is restricted; another user's needs
    // CAP_DAC_READ_SEARCH plus CAP_SYS_PTRACE (root with its normal capabilities; see parseProcessIo()).
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
    // /proc/<pid>/cgroup names the process's cgroups: the v2 "0::<path>" line and/or v1 lines,
    // including the freezer controller's. isCgroupFrozen() checks the matching freeze state.
    // Read to EOF: a cgroup path can approach PATH_MAX, and a truncated one would point the check
    // at the wrong cgroup.events (#1228 review).
    const auto cgroupPath = (procRoot / std::to_string(pid) / "cgroup").string();
    const std::vector<char> cgroupContents = ProcParsing::readProcFileFull(cgroupPath.c_str());
    if (!cgroupContents.empty() &&
        CgroupPath::isCgroupFrozen(std::string_view(cgroupContents.data(), cgroupContents.size()), std::filesystem::path("/sys/fs/cgroup")))
    {
        return "Suspended";
    }

    // No special status
    return {};
}
uint64_t LinuxProcessProbe::readTotalCpuTime() const
{
    const auto times = readCpuTimes();
    return times.has_value() ? times->total : 0;
}

std::optional<LinuxProcessProbe::CpuTimes> LinuxProcessProbe::readCpuTimes() const
{
    // Format: /proc/stat — first line: "cpu user nice system idle iowait irq softirq steal …"
    // We only need the first line, so 256 bytes is ample.

    const std::string statPath = (m_ProcRoot / "stat").string();
    std::array<char, 256> buf{};
    const std::size_t len = readProcFile(statPath.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        spdlog::warn("Failed to open {}", statPath);
        return std::nullopt;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;

    // First line must start with "cpu "
    if (len < 4 || p[0] != 'c' || p[1] != 'p' || p[2] != 'u' || p[3] != ' ')
    {
        spdlog::warn("Failed to parse {}", statPath);
        return std::nullopt;
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
        return std::nullopt;
    }

    // Total CPU time = all fields combined. Busy = the time processes' own utime + stime account
    // for (user, nice and system), the interval denominator for energy attribution (#1093).
    return CpuTimes{.total = user + nice + system + idle + iowait + irq + softirq + steal, .busy = user + nice + system};
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
    // energy_uj has been root-only (0400) since the Platypus fix (CVE-2020-8694), so a file that
    // exists is not enough: detection used to accept one by existence and then report 0 W for
    // every process for a normal user (#1103). Only a file we can open counts.
    const auto isReadable = [](const std::filesystem::path& path)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1)
        {
            return false;
        }
        ::close(fd);
        return true;
    };

    std::vector<std::filesystem::path> candidates = {
        m_PowercapRoot / "intel-rapl" / "intel-rapl:0" / "energy_uj",
        m_PowercapRoot / "intel-rapl:0" / "energy_uj",
    };
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_PowercapRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().filename().string().starts_with("intel-rapl"))
        {
            candidates.push_back(it->path() / "energy_uj");
            candidates.push_back(it->path() / "intel-rapl:0" / "energy_uj");
        }
    }

    for (const auto& path : candidates)
    {
        if (!isReadable(path))
        {
            continue;
        }
        m_PowerCapPath = path.string();

        // Where the counter wraps back to 0, so a wrap reads as the energy used, not a drop.
        std::array<char, 32> buf{};
        const std::string rangePath = (path.parent_path() / "max_energy_range_uj").string();
        if (const std::size_t len = readProcFile(rangePath.c_str(), buf.data(), buf.size()); len > 0)
        {
            const char* p = buf.data();
            if (!parseNum(p, buf.data() + len, m_PowerCapMaxRangeUj))
            {
                m_PowerCapMaxRangeUj = 0;
            }
        }
        return true;
    }

    return false;
}

std::optional<uint64_t> LinuxProcessProbe::readSystemEnergy() const
{
    if (m_PowerCapPath.empty())
    {
        return std::nullopt;
    }

    std::array<char, 32> buf{};
    const std::size_t len = readProcFile(m_PowerCapPath.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return std::nullopt;
    }

    const char* p = buf.data();
    uint64_t energyUj = 0;
    if (!parseNum(p, buf.data() + len, energyUj))
    {
        return std::nullopt;
    }

    return energyUj; // Already in microjoules
}

std::optional<PackageEnergyReading> LinuxProcessProbe::readPackageEnergy() const
{
    if (!m_HasPowerCap)
    {
        return std::nullopt;
    }
    // Raw read only: ProcessModel shares it out between processes per interval (#1093).
    const auto cpuTimes = readCpuTimes();
    return PackageEnergyReading{.energyUj = readSystemEnergy(),
                                .maxRangeUj = m_PowerCapMaxRangeUj,
                                .busyCpuTicks = cpuTimes.has_value() ? std::optional{cpuTimes->busy} : std::nullopt};
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
SocketTrafficReading LinuxProcessProbe::readSocketTraffic() const
{
    if (!m_HasNetworkCounters)
    {
        return {};
    }
    // Copy out a stable local reference: if setSocketStatsCacheTtl() swaps m_SocketStats
    // concurrently, this call keeps using the instance it started with (kept alive by
    // this shared_ptr) rather than racing the reassignment.
    const auto stats = socketStats();
    if (!stats)
    {
        return {};
    }

    // Query all TCP sockets with their byte counters. A failed or partial dump comes back empty
    // with sampledAt unset (#1160) and is reported as no reading (sampleTimeNs 0), not as an empty
    // one: a socket missing from it would look closed and then, back in the next reading, new.
    // The query is cached (DEFAULT_SOCKET_STATS_CACHE_TTL), so the same reading -- with the same
    // sampledAt -- can come back for several calls; Domain folds it once.
    std::chrono::steady_clock::time_point sampledAt;
    const std::vector<SocketStats> sockets = stats->queryAllSockets(&sampledAt);
    if (sampledAt == std::chrono::steady_clock::time_point{})
    {
        return {};
    }

    SocketTrafficReading reading;
    reading.sampleTimeNs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sampledAt.time_since_epoch()).count());
    if (sockets.empty())
    {
        // A complete reading with no sockets: every connection closed, so none is unowned any more.
        // Forget them: a later connection reusing one of their inodes would otherwise inherit its
        // first-seen time, look older than the map, and not trigger the early rebuild (#1327 review).
        // A failed reading (returned above) leaves them alone: its sockets are unknown, not closed.
        const std::scoped_lock lock{m_UnownedSocketsMutex};
        m_UnownedSocketsFirstSeen.clear();
        return reading;
    }

    // Attribute each socket to the process holding it (socket inode -> PID, from /proc/[pid]/fd).
    // A socket not in the map (opened since its last rebuild, or held by a process we can't read)
    // is still reported, unattributed, so Domain tracks its counters from now on.
    // The owner comes with its start time, so Domain can tell it from a process that reused its PID (#1336).
    const auto ownerOf = [](const InodeToPidMap* inodeToPid, std::uint64_t inode) -> SocketOwner
    {
        if (inodeToPid != nullptr)
        {
            if (const auto it = inodeToPid->find(inode); it != inodeToPid->end())
            {
                return it->second;
            }
        }
        return {};
    };
    auto snapshot = currentInodeToPidMap(std::chrono::milliseconds{Domain::Sampling::INODE_PID_CACHE_TTL_MS});

    bool unownedSinceBuild = false;
    {
        const std::scoped_lock lock{m_UnownedSocketsMutex};
        // A socket with no owner that wasn't already unowned in a reading taken before the map was
        // built may have been opened since the build: rebuild early (rate-limited) so a new
        // connection is attributed in the reading it first appears in, rather than up to a TTL later
        // with its first bytes, or all of a short one's, never credited (#1259). Sockets held by
        // processes we can't read stay unowned through the rebuild and so don't trigger another.
        // The rebuild may land on a cached socket query (same sampledAt): the reading then differs
        // from the last only in ownership, which Domain still applies (SocketTrafficAccumulator).
        unownedSinceBuild = std::ranges::any_of(sockets,
                                                [&](const SocketStats& socket)
                                                {
                                                    if (ownerOf(snapshot.map.get(), socket.inode).pid != 0)
                                                    {
                                                        return false;
                                                    }
                                                    const auto seen = m_UnownedSocketsFirstSeen.find(socket.inode);
                                                    const auto firstSeen =
                                                        (seen != m_UnownedSocketsFirstSeen.end()) ? seen->second : sampledAt;
                                                    return firstSeen > snapshot.builtAt;
                                                });
    }
    // The early rebuild may scan every /proc/[pid]/fd: do it without holding m_UnownedSocketsMutex.
    if (unownedSinceBuild)
    {
        snapshot = currentInodeToPidMap(m_InodeMapEarlyRebuildInterval);
    }

    {
        const std::scoped_lock lock{m_UnownedSocketsMutex};
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> unowned;
        reading.sockets.reserve(sockets.size());
        for (const auto& socket : sockets)
        {
            const SocketOwner owner = ownerOf(snapshot.map.get(), socket.inode);
            if (owner.pid == 0)
            {
                const auto seen = m_UnownedSocketsFirstSeen.find(socket.inode);
                unowned.insert_or_assign(socket.inode, (seen != m_UnownedSocketsFirstSeen.end()) ? seen->second : sampledAt);
            }
            reading.sockets.push_back(SocketTrafficSample{.key = socket.inode,
                                                          .pid = owner.pid,
                                                          .ownerStartTimeTicks = owner.startTimeTicks,
                                                          .bytesReceived = socket.bytesReceived,
                                                          .bytesSent = socket.bytesSent});
        }
        m_UnownedSocketsFirstSeen = std::move(unowned);
    }
    return reading;
}

LinuxProcessProbe::InodeToPidSnapshot LinuxProcessProbe::currentInodeToPidMap(std::chrono::milliseconds maxAge) const
{
    // Refresh inode-to-PID map on a TTL basis to avoid scanning /proc/[pid]/fd/* every
    // enumerate(). The rebuild slot is claimed by advancing m_InodeToPidCacheTime under
    // the initial lock, so only one thread rebuilds per TTL window while all others
    // continue using the previous shared_ptr snapshot (see #460).
    InodeToPidSnapshot snapshot;
    bool needsRebuild = false;
    std::chrono::steady_clock::time_point scanStart;
    {
        const std::scoped_lock lock{m_InodePidCacheMutex};
        const auto now = std::chrono::steady_clock::now();
        // However the map got stale, never scan more often than the early-rebuild interval: an empty
        // scan backdates m_InodeToPidCacheTime for a quick retry, and that retry must not combine
        // with an early rebuild into two scans per reading.
        needsRebuild = (now - m_InodeToPidCacheTime) >= maxAge && (now - m_InodeToPidLastAttempt) >= m_InodeMapEarlyRebuildInterval;
        if (needsRebuild)
        {
            // Claim the rebuild slot: advance the timestamps now so any other thread that
            // checks while we are scanning /proc sees a fresh time and skips rebuilding.
            m_InodeToPidCacheTime = now;
            m_InodeToPidLastAttempt = now;
            scanStart = now;
        }
        snapshot = {.map = m_InodeToPidCache, .builtAt = m_InodeToPidBuiltAt}; // current (possibly stale) snapshot
    }
    if (needsRebuild)
    {
        // Build the map outside the lock; concurrent threads keep using the old snapshot.
        if (m_InodeMapScanHook)
        {
            m_InodeMapScanHook();
        }
        auto rebuilt = std::make_shared<const InodeToPidMap>(buildInodeToPidMap(m_ProcRoot));
        {
            const std::scoped_lock lock{m_InodePidCacheMutex};
            if (!rebuilt->empty())
            {
                m_InodeToPidCache = std::move(rebuilt);
                m_InodeToPidCacheTime = std::chrono::steady_clock::now();
                m_InodeToPidBuiltAt = scanStart;
                snapshot = {.map = m_InodeToPidCache, .builtAt = m_InodeToPidBuiltAt};
            }
            else
            {
                // Preserve the previous snapshot when procfs enumeration transiently
                // produces no entries; allow a quick retry instead of waiting the full TTL
                // (still no sooner than m_InodeMapEarlyRebuildInterval after this attempt).
                constexpr auto EMPTY_REBUILD_RETRY_MS = std::chrono::milliseconds{100};
                const auto ttl = std::chrono::milliseconds{Domain::Sampling::INODE_PID_CACHE_TTL_MS};
                const auto retryDelay = std::min(EMPTY_REBUILD_RETRY_MS, ttl);
                m_InodeToPidCacheTime = std::chrono::steady_clock::now() - (ttl - retryDelay);
                // The scan still tried to resolve every socket unowned before it: advance builtAt so
                // those sockets don't count as opened since the build and trigger an early rebuild
                // every interval (#1259).
                m_InodeToPidBuiltAt = scanStart;
                snapshot = {.map = m_InodeToPidCache, .builtAt = m_InodeToPidBuiltAt};
            }
        }
    }
    return snapshot;
}
#endif // TASKSMACK_HAS_NETLINK_SOCKET_STATS

} // namespace Platform

#endif
