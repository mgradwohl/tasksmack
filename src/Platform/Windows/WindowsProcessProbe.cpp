#include "WindowsProcessProbe.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
// clang-format on

#include <string>

namespace Platform
{

namespace
{

/// Convert FILETIME to 100-nanosecond intervals (ticks)
[[nodiscard]] uint64_t filetimeToTicks(const FILETIME& ft)
{
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

/// Convert wide string to UTF-8
[[nodiscard]] std::string wideToUtf8(const wchar_t* wide)
{
    if (wide == nullptr || wide[0] == L'\0')
    {
        return {};
    }

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(sizeNeeded - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

/// Map Windows process state to single character
[[nodiscard]] char getProcessState(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return '?';
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess, &exitCode) != 0)
    {
        if (exitCode == STILL_ACTIVE)
        {
            return 'R'; // Running
        }
        return 'Z'; // Zombie/terminated
    }
    return '?';
}

} // namespace

WindowsProcessProbe::WindowsProcessProbe()
{
    spdlog::debug("WindowsProcessProbe initialized");
}

std::vector<ProcessCounters> WindowsProcessProbe::enumerate()
{
    std::vector<ProcessCounters> results;

    // Create snapshot of all processes
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        spdlog::error("CreateToolhelp32Snapshot failed: {}", GetLastError());
        return results;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(pe32);

    if (Process32FirstW(hSnapshot, &pe32) == 0)
    {
        CloseHandle(hSnapshot);
        return results;
    }

    do
    {
        ProcessCounters counters{};
        counters.pid = static_cast<int32_t>(pe32.th32ProcessID);
        counters.parentPid = static_cast<int32_t>(pe32.th32ParentProcessID);
        counters.name = wideToUtf8(pe32.szExeFile);
        counters.threadCount = static_cast<int32_t>(pe32.cntThreads);

        // Get detailed info (CPU times, memory) - may fail for protected processes
        // Ignore return value - we still want to include process even if details fail
        (void)getProcessDetails(pe32.th32ProcessID, counters);

        results.push_back(std::move(counters));
    } while (Process32NextW(hSnapshot, &pe32) != 0);

    CloseHandle(hSnapshot);

    spdlog::trace("Enumerated {} processes", results.size());
    return results;
}

bool WindowsProcessProbe::getProcessDetails(uint32_t pid, ProcessCounters& counters) const
{
    // Open process with limited access - some system processes won't allow full access
    HANDLE hProcess =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);

    if (hProcess == nullptr)
    {
        // Can't access this process - leave defaults
        counters.state = '?';
        return false;
    }

    // Get process state
    counters.state = getProcessState(hProcess);

    // Get CPU times
    FILETIME ftCreation{};
    FILETIME ftExit{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetProcessTimes(hProcess, &ftCreation, &ftExit, &ftKernel, &ftUser) != 0)
    {
        // Convert to ticks (100-nanosecond intervals)
        counters.userTime = filetimeToTicks(ftUser);
        counters.systemTime = filetimeToTicks(ftKernel);
        counters.startTimeTicks = filetimeToTicks(ftCreation);
    }

    // Get memory info
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);

    if (GetProcessMemoryInfo(hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)) != 0)
    {
        counters.rssBytes = pmc.WorkingSetSize;
        counters.virtualBytes = pmc.PrivateUsage;
    }

    // Get I/O counters
    IO_COUNTERS ioCounters{};
    if (GetProcessIoCounters(hProcess, &ioCounters) != 0)
    {
        counters.readBytes = ioCounters.ReadTransferCount;
        counters.writeBytes = ioCounters.WriteTransferCount;
    }

    CloseHandle(hProcess);
    return true;
}

ProcessCapabilities WindowsProcessProbe::capabilities() const
{
    return ProcessCapabilities{
        .hasIoCounters = true,
        .hasThreadCount = true,
        .hasUserSystemTime = true,
        .hasStartTime = true,
    };
}

uint64_t WindowsProcessProbe::totalCpuTime() const
{
    return readTotalCpuTime();
}

uint64_t WindowsProcessProbe::readTotalCpuTime() const
{
    FILETIME ftIdle{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser) == 0)
    {
        spdlog::error("GetSystemTimes failed: {}", GetLastError());
        return 0;
    }

    // Total = kernel + user (kernel includes idle time)
    return filetimeToTicks(ftKernel) + filetimeToTicks(ftUser);
}

long WindowsProcessProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    // 10,000,000 ticks per second
    return 10'000'000L;
}

} // namespace Platform
