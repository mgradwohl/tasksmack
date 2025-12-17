#include "LinuxProcessProbe.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include <dirent.h>
#include <pwd.h>
#include <unistd.h>

namespace Platform
{

namespace
{

/// Cache UID to username mappings to avoid repeated getpwuid calls
std::unordered_map<uid_t, std::string>& getUsernameCache()
{
    static std::unordered_map<uid_t, std::string> cache;
    return cache;
}

/// Get username from UID, with caching
[[nodiscard]] std::string getUsername(uid_t uid)
{
    auto& cache = getUsernameCache();
    auto it = cache.find(uid);
    if (it != cache.end())
    {
        return it->second;
    }

    // Look up username from passwd database
    struct passwd* pwd = getpwuid(uid);
    std::string username;
    if (pwd != nullptr && pwd->pw_name != nullptr)
    {
        username = pwd->pw_name;
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

LinuxProcessProbe::LinuxProcessProbe() : m_TicksPerSecond(sysconf(_SC_CLK_TCK)), m_PageSize(sysconf(_SC_PAGESIZE))
{
    if (m_TicksPerSecond <= 0)
    {
        m_TicksPerSecond = 100; // Common default
        spdlog::warn("Failed to get CLK_TCK, using default: {}", m_TicksPerSecond);
    }
    if (m_PageSize <= 0)
    {
        m_PageSize = 4096; // Common default
        spdlog::warn("Failed to get PAGE_SIZE, using default: {}", m_PageSize);
    }
}

std::vector<ProcessCounters> LinuxProcessProbe::enumerate()
{
    std::vector<ProcessCounters> processes;
    processes.reserve(500); // Reasonable initial size

    std::filesystem::path procPath("/proc");
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
        if (parseProcessStat(pid, counters))
        {
            parseProcessStatm(pid, counters);
            parseProcessStatus(pid, counters);
            parseProcessCmdline(pid, counters);
            processes.push_back(std::move(counters));
        }
    }

    if (errorCode)
    {
        spdlog::warn("Error iterating /proc: {}", errorCode.message());
    }

    return processes;
}

ProcessCapabilities LinuxProcessProbe::capabilities() const
{
    return ProcessCapabilities{.hasIoCounters = false, // Would need /proc/[pid]/io (requires root)
                               .hasThreadCount = true,
                               .hasUserSystemTime = true,
                               .hasStartTime = true,
                               .hasUser = true,    // From /proc/[pid]/status Uid field
                               .hasCommand = true, // From /proc/[pid]/cmdline
                               .hasNice = true};   // From /proc/[pid]/stat
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

    std::filesystem::path statPath = std::filesystem::path("/proc") / std::to_string(pid) / "stat";
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
    counters.threadCount = static_cast<int32_t>(numThreads > 0 ? numThreads : 1);
    counters.startTimeTicks = starttime;
    counters.virtualBytes = vsize;
    counters.rssBytes = static_cast<uint64_t>(rss) * static_cast<uint64_t>(m_PageSize);
    counters.nice = static_cast<int32_t>(nice);

    return true;
}

void LinuxProcessProbe::parseProcessStatm(int32_t pid, ProcessCounters& counters) const
{
    // Format: /proc/[pid]/statm
    // Fields: size resident shared text lib data dt (all in pages)

    std::filesystem::path statmPath = std::filesystem::path("/proc") / std::to_string(pid) / "statm";
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
        counters.rssBytes = resident * static_cast<uint64_t>(m_PageSize);
        counters.sharedBytes = shared * static_cast<uint64_t>(m_PageSize);
    }
}

void LinuxProcessProbe::parseProcessStatus(int32_t pid, ProcessCounters& counters) const
{
    // Read /proc/[pid]/status for UID (owner) information
    // Format is key:value pairs, one per line
    // We need: Uid: <real> <effective> <saved> <filesystem>

    std::filesystem::path statusPath = std::filesystem::path("/proc") / std::to_string(pid) / "status";
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

void LinuxProcessProbe::parseProcessCmdline(int32_t pid, ProcessCounters& counters) const
{
    // Format: /proc/[pid]/cmdline
    // Arguments are separated by null bytes

    std::filesystem::path cmdlinePath = std::filesystem::path("/proc") / std::to_string(pid) / "cmdline";
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

uint64_t LinuxProcessProbe::readTotalCpuTime() const
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

} // namespace Platform
