// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<pwd.h>) && __has_include(<unistd.h>)

#include "LinuxProcessProbe.h"

// Include NetlinkSocketStats only when its headers are available
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "NetlinkSocketStats.h"
#endif

#include <spdlog/spdlog.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>

#include <dirent.h>
#include <pwd.h>
#include <sched.h>
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

LinuxProcessProbe::LinuxProcessProbe() : m_TicksPerSecond(sysconf(_SC_CLK_TCK)), m_PageSize(toU64PositiveOr(sysconf(_SC_PAGESIZE), 4096ULL))
{
    if (m_TicksPerSecond <= 0)
    {
        m_TicksPerSecond = 100; // Common default
        spdlog::warn("Failed to get CLK_TCK, using default: {}", m_TicksPerSecond);
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

std::vector<ProcessCounters> LinuxProcessProbe::enumerate()
{
    std::vector<ProcessCounters> processes;
    processes.reserve(500); // Reasonable initial size

    const std::filesystem::path procPath("/proc");
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
            spdlog::debug("Failed to parse /proc/{}/stat", pid);
            continue;
        }

        parseProcessStatm(pid, counters);
        parseProcessStatus(pid, counters);
        parseProcessCmdline(pid, counters);
        // CPU affinity is always safe to query; failures zero the mask
        parseProcessAffinity(pid, counters);

        // Only attempt I/O counters if we know they're readable
        // Use std::call_once for thread-safe lazy initialization
        std::call_once(m_IoCountersCheckFlag, [this]() { m_IoCountersAvailable = checkIoCountersAvailability(); });
        if (m_IoCountersAvailable)
        {
            parseProcessIo(pid, counters);
        }
        counters.status = getProcessStatus(pid); // Get cgroup freezer status
        processes.push_back(std::move(counters));
    }

    if (errorCode)
    {
        spdlog::warn("Error iterating /proc: {}", errorCode.message());
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
    std::call_once(m_IoCountersCheckFlag, [this]() { m_IoCountersAvailable = checkIoCountersAvailability(); });

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    const bool hasNetworkCounters = m_HasNetworkCounters;
#else
    const bool hasNetworkCounters = false;
#endif

    return ProcessCapabilities{.hasIoCounters = m_IoCountersAvailable,
                               .hasThreadCount = true,
                               .hasUserSystemTime = true,
                               .hasStartTime = true,
                               .hasUser = true,       // From /proc/[pid]/status Uid field
                               .hasCommand = true,    // From /proc/[pid]/cmdline
                               .hasNice = true,       // From /proc/[pid]/stat
                               .hasPageFaults = true, // From /proc/[pid]/stat (minflt + majflt)
                               .hasPeakRss = false,
                               .hasCpuAffinity = true,                   // From sched_getaffinity
                               .hasNetworkCounters = hasNetworkCounters, // From Netlink INET_DIAG (if available)
                               .hasPowerUsage = m_HasPowerCap,           // Available if RAPL is detected
                               .hasStatus = true};                       // From cgroup freezer state
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
    // Format: /proc/[pid]/stat
    // Fields: pid (comm) state ppid pgrp session tty_nr tpgid flags
    //         minflt cminflt majflt cmajflt utime stime cutime cstime
    //         priority nice num_threads itrealvalue starttime vsize rss ...

    const std::filesystem::path statPath = std::filesystem::path("/proc") / std::to_string(pid) / "stat";
    std::ifstream statFile(statPath);
    if (!statFile.is_open())
    {
        return false;
    }

    std::string line;
    if (!std::getline(statFile, line))
    {
        return false;
    }

    // Process name is in parentheses and may contain spaces or parentheses
    // Find the last ')' to handle names like "process (name)"
    const auto nameStart = line.find('(');
    const auto nameEnd = line.rfind(')');

    if (nameStart == std::string::npos || nameEnd == std::string::npos || nameEnd <= nameStart)
    {
        return false;
    }

    counters.pid = pid;
    counters.name = line.substr(nameStart + 1, nameEnd - nameStart - 1);

    // Parse fields after the name
    std::istringstream fieldStream(line.substr(nameEnd + 2)); // Skip ") "

    std::string stateStr;
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

    // clang-format off
    fieldStream >> stateStr >> parentPid >> pgrp >> session >> ttyNr >> tpgid
                >> flags >> minflt >> cminflt >> majflt >> cmajflt
                >> utime >> stime >> cutime >> cstime >> priority >> nice
                >> numThreads >> itrealvalue >> starttime >> vsize >> rss;
    // clang-format on

    if (fieldStream.fail())
    {
        return false;
    }

    counters.state = stateStr.empty() ? '?' : stateStr[0];
    counters.parentPid = parentPid;
    counters.userTime = utime;
    counters.systemTime = stime;
    counters.threadCount = clampToI32((numThreads > 0) ? numThreads : 1);
    counters.startTimeTicks = starttime;
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

    const std::filesystem::path statmPath = std::filesystem::path("/proc") / std::to_string(pid) / "statm";
    std::ifstream statmFile(statmPath);
    if (!statmFile.is_open())
    {
        return;
    }

    uint64_t size = 0;
    uint64_t resident = 0;
    uint64_t shared = 0;

    statmFile >> size >> resident >> shared;
    if (!statmFile.fail())
    {
        // statm gives more accurate RSS, update if available
        counters.rssBytes = resident * m_PageSize;
        counters.sharedBytes = shared * m_PageSize;
    }
}

void LinuxProcessProbe::parseProcessStatus(int32_t pid, ProcessCounters& counters)
{
    // Read /proc/[pid]/status for UID (owner) information
    // Format is key:value pairs, one per line
    // We need: Uid: <real> <effective> <saved> <filesystem>

    const std::filesystem::path statusPath = std::filesystem::path("/proc") / std::to_string(pid) / "status";
    std::ifstream statusFile(statusPath);
    if (!statusFile.is_open())
    {
        return;
    }

    std::string line;
    while (std::getline(statusFile, line))
    {
        // Look for "Uid:" line
        if (line.starts_with("Uid:"))
        {
            std::istringstream iss(line.substr(4)); // Skip "Uid:"
            uid_t realUid = 0;
            iss >> realUid;
            if (!iss.fail())
            {
                counters.user = getUsername(realUid);
            }
            break;
        }
    }
}

void LinuxProcessProbe::parseProcessCmdline(int32_t pid, ProcessCounters& counters)
{
    // Format: /proc/[pid]/cmdline
    // Arguments are separated by null bytes

    const std::filesystem::path cmdlinePath = std::filesystem::path("/proc") / std::to_string(pid) / "cmdline";
    std::ifstream cmdlineFile(cmdlinePath, std::ios::binary);
    if (!cmdlineFile.is_open())
    {
        return;
    }

    std::string cmdline;
    std::getline(cmdlineFile, cmdline, '\0');

    // Read remaining arguments
    std::string arg;
    while (std::getline(cmdlineFile, arg, '\0') && !arg.empty())
    {
        cmdline += ' ';
        cmdline += arg;
    }

    // Some processes (like kernel threads) have empty cmdline - use name instead
    if (cmdline.empty())
    {
        counters.command = "[" + counters.name + "]";
    }
    else
    {
        counters.command = std::move(cmdline);
    }
}

void LinuxProcessProbe::parseProcessAffinity(int32_t pid, ProcessCounters& counters)
{
    // Use sched_getaffinity to read CPU affinity mask for the process
    // This returns which CPU cores the process is allowed to run on

    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);

    // sched_getaffinity returns the affinity for the main thread of the process
    if (sched_getaffinity(pid, sizeof(cpu_set_t), &cpuSet) == 0)
    {
        // Convert cpu_set_t to a bitmask that fits in uint64_t
        // This limits us to 64 cores, which is reasonable for most systems
        std::uint64_t mask = 0;
        for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; ++cpu)
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

void LinuxProcessProbe::parseProcessIo(int32_t pid, ProcessCounters& counters)
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

    const std::filesystem::path ioPath = std::filesystem::path("/proc") / std::to_string(pid) / "io";
    std::ifstream ioFile(ioPath);
    if (!ioFile.is_open())
    {
        // Common case: insufficient permissions, just return
        return;
    }

    std::string line;
    while (std::getline(ioFile, line))
    {
        constexpr std::string_view readPrefix = "read_bytes:";
        constexpr std::string_view writePrefix = "write_bytes:";

        if (line.starts_with(readPrefix))
        {
            std::istringstream iss(line.substr(readPrefix.length()));
            uint64_t readBytes = 0;
            iss >> readBytes;
            if (!iss.fail())
            {
                counters.readBytes = readBytes;
            }
        }
        else if (line.starts_with(writePrefix))
        {
            std::istringstream iss(line.substr(writePrefix.length()));
            uint64_t writeBytes = 0;
            iss >> writeBytes;
            if (!iss.fail())
            {
                counters.writeBytes = writeBytes;
            }
        }
    }
}

bool LinuxProcessProbe::checkIoCountersAvailability()
{
    // Check if we can read /proc/self/io to determine I/O counter availability.
    // This file requires CAP_DAC_READ_SEARCH capability or root privileges,
    // or being the owner of the target process.
    const std::ifstream selfIo("/proc/self/io");
    return selfIo.is_open();
}

std::string LinuxProcessProbe::getProcessStatus(int32_t pid)
{
    // Try cgroup v2 first: freezer.state
    const std::filesystem::path cgroupV2FreezerPath = std::filesystem::path("/sys/fs/cgroup") / std::to_string(pid) / "freezer.state";
    std::ifstream freezerStateV2(cgroupV2FreezerPath);
    if (freezerStateV2.is_open())
    {
        std::string state;
        freezerStateV2 >> state;
        if (state == "FROZEN" || state == "FREEZING")
        {
            return "Suspended";
        }
    }

    // Fallback to cgroup v1 freezer hierarchy
    // /proc/[pid]/cgroup lists all cgroups for the process
    const std::filesystem::path cgroupPath = std::filesystem::path("/proc") / std::to_string(pid) / "cgroup";
    std::ifstream cgroupFile(cgroupPath);
    if (cgroupFile.is_open())
    {
        std::string line;
        while (std::getline(cgroupFile, line))
        {
            // Format: hierarchy-ID:controllers:cgroup-path
            const auto firstColon = line.find(':');
            const auto secondColon = line.find(':', firstColon + 1);
            if (firstColon != std::string::npos && secondColon != std::string::npos)
            {
                const std::string controllers = line.substr(firstColon + 1, secondColon - firstColon - 1);
                const std::string cgroupSubPath = line.substr(secondColon + 1);

                // Check if this line has the freezer controller
                if (controllers.contains("freezer"))
                {
                    // Build path: /sys/fs/cgroup/freezer/<cgroup-path>/freezer.state
                    // Skip if cgroupSubPath is empty or doesn't start with /
                    if (!cgroupSubPath.empty() && cgroupSubPath[0] == '/')
                    {
                        const std::filesystem::path freezePathV1 =
                            std::filesystem::path("/sys/fs/cgroup/freezer") / cgroupSubPath.substr(1) / "freezer.state";
                        std::ifstream freezeFileV1(freezePathV1);
                        if (freezeFileV1.is_open())
                        {
                            std::string state;
                            freezeFileV1 >> state;
                            if (state == "FROZEN" || state == "FREEZING")
                            {
                                return "Suspended";
                            }
                        }
                    }
                }
            }
        }
    }

    // No special status
    return {};
}
uint64_t LinuxProcessProbe::readTotalCpuTime()
{
    // Format: /proc/stat
    // First line: cpu user nice system idle iowait irq softirq steal guest guest_nice

    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open())
    {
        spdlog::warn("Failed to open /proc/stat");
        return 0;
    }

    std::string cpuLabel;
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;

    statFile >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    if (statFile.fail() || cpuLabel != "cpu")
    {
        spdlog::warn("Failed to parse /proc/stat");
        return 0;
    }

    // Total CPU time = all fields combined
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

uint64_t LinuxProcessProbe::systemTotalMemory() const
{
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open())
    {
        spdlog::error("Failed to open /proc/meminfo");
        return 0;
    }

    std::string line;
    while (std::getline(meminfo, line))
    {
        if (line.starts_with("MemTotal:"))
        {
            const auto colonPos = line.find(':');
            if (colonPos == std::string::npos)
            {
                continue;
            }

            const char* begin = line.data() + colonPos + 1;
            const char* const end = line.data() + line.size();
            while (begin < end && (*begin == ' ' || *begin == '\t'))
            {
                ++begin;
            }

            uint64_t kb = 0;
            const auto [ptr, ec] = std::from_chars(begin, end, kb);
            if (ec == std::errc())
            {
                (void) ptr;
                return kb * 1024ULL;
            }
        }
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
        const std::ifstream file(path);
        if (file.good())
        {
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

    std::ifstream file(m_PowerCapPath);
    if (!file.is_open())
    {
        return 0;
    }

    uint64_t energyUj = 0;
    file >> energyUj;

    if (file.fail())
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

    // Build inode-to-PID mapping by scanning /proc/[pid]/fd/*
    // TODO: Consider caching this mapping with a TTL to reduce /proc scanning overhead
    //       on systems with many processes. Current approach scans on each enumerate()
    //       call (~1Hz) which may add latency on systems with thousands of processes.
    const std::unordered_map<std::uint64_t, std::int32_t> inodeToPid = buildInodeToPidMap();
    if (inodeToPid.empty())
    {
        return;
    }

    // Aggregate socket bytes by PID
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
