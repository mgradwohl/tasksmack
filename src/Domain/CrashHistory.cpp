#include "CrashHistory.h"

#include "Domain/ProcessModules.h"
#include "Platform/AsciiCase.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
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
    std::uint64_t startedAt = 0;
    std::uint64_t order = 0;
    {
        const std::scoped_lock lock(m_ReadMutex);
        // When this read starts, once it has the read lock: publish() keeps the newest read by its
        // ticket, so a slower read that started earlier (this one or the System Information read) can't
        // replace a newer list (#1773). The wall-clock time is only shown: it can step backwards (#2157).
        order = beginRead();
        startedAt = nowUnixSeconds();
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
        catch (...)
        {
            // Not a std::exception: published the same way, so nothing is rethrown to the reader's
            // caller on another thread (#1706).
            crashes = {};
            crashes.available = true;
            crashes.unavailableReason = "Couldn't read the crash history: unknown error";
        }
    }
    publish(std::move(crashes), startedAt, order);
}

void CrashHistory::publish(Platform::CrashesInfo crashes, std::uint64_t readAtUnixSeconds, std::uint64_t readOrder)
{
    auto next = std::make_shared<CrashHistorySnapshot>();
    next->crashes = std::move(crashes);
    next->readAtUnixSeconds = readAtUnixSeconds;

    const std::scoped_lock lock(m_PublishMutex);
    if (m_LastVersion != 0 && readOrder < m_LastReadOrder)
    {
        return; // a slower read that started before the one already shown
    }
    m_LastReadOrder = readOrder;
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
        // Windows file names are case-insensitive: "NOTEPAD.EXE" is notepad.exe. ASCII letters only,
        // so no locale folds a byte of UTF-8 (#2566).
        return Platform::equalsIgnoreCase(application, name);
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
