#include "CrashHistory.h"

#include "Domain/ProcessModules.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Domain
{

namespace
{

[[nodiscard]] std::uint64_t nowUnixSeconds() noexcept
{
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}

[[nodiscard]] bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a,
                              b,
                              [](char x, char y)
                              { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace

CrashHistory::CrashHistory(ReadFn read) : m_Read(std::move(read))
{
    if (!m_Read)
    {
        throw std::invalid_argument("CrashHistory requires a read function");
    }
}

void CrashHistory::read()
{
    Platform::CrashesInfo crashes;
    {
        const std::scoped_lock lock(m_ReadMutex);
        try
        {
            crashes = m_Read();
        }
        catch (const std::exception& e)
        {
            // Copied here, on the thread that threw: only a plain value is published (#1685).
            crashes = {};
            crashes.available = true;
            crashes.unavailableReason = std::string("Couldn't read the crash history: ") + e.what();
        }
    }
    publish(std::move(crashes), nowUnixSeconds());
}

void CrashHistory::publish(Platform::CrashesInfo crashes, std::uint64_t readAtUnixSeconds)
{
    auto next = std::make_shared<CrashHistorySnapshot>();
    next->crashes = std::move(crashes);
    next->readAtUnixSeconds = readAtUnixSeconds;

    const std::scoped_lock lock(m_PublishMutex);
    if (m_LastVersion != 0 && readAtUnixSeconds < m_LastReadAtSeconds)
    {
        return; // a slower read that started before the one already shown
    }
    m_LastReadAtSeconds = readAtUnixSeconds;
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

bool crashMatchesExecutable(const Platform::CrashEvent& event, std::string_view executable, Platform::OsFamily family)
{
    const std::string_view name = Modules::fileName(executable);
    if (name.empty())
    {
        return false;
    }
    const std::string_view application = Modules::fileName(event.application);
    switch (family)
    {
    case Platform::OsFamily::Windows:
        // Windows file names are case-insensitive: "NOTEPAD.EXE" is notepad.exe.
        return equalsIgnoreAsciiCase(application, name);
    case Platform::OsFamily::Linux:
        // A core is named by its process's comm, cut by the kernel to 15 bytes; the journal's
        // executable path (#1674), when there is one, has the whole name.
        if (!event.executable.empty() && Modules::fileName(event.executable) == name)
        {
            return true;
        }
        return !application.empty() && application == name.substr(0, LINUX_COMM_MAX_BYTES);
    case Platform::OsFamily::Unknown:
        break;
    }
    return application == name;
}

ExecutableCrashCounts crashCountsFor(const Platform::CrashesInfo& crashes, std::string_view executable)
{
    ExecutableCrashCounts counts;
    counts.truncatedName = crashes.family == Platform::OsFamily::Linux && Modules::fileName(executable).size() > LINUX_COMM_MAX_BYTES;
    if (!crashes.listed)
    {
        return counts;
    }
    for (const Platform::CrashEvent& event : crashes.events)
    {
        if (!crashMatchesExecutable(event, executable, crashes.family))
        {
            continue;
        }
        if (event.hang)
        {
            ++counts.hangs;
        }
        else
        {
            ++counts.crashes;
        }
        counts.lastUnixSeconds = std::max(counts.lastUnixSeconds, event.unixSeconds);
        if (counts.recent.size() < CRASH_RECENT_MAX)
        {
            counts.recent.push_back(event);
        }
    }
    return counts;
}

} // namespace Domain
