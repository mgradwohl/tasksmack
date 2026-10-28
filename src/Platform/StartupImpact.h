#pragma once

// A startup entry's share of a sign-in's startup measurement (#3073): which of the measured processes
// are its program's, and what they and the processes they started cost. Standard library only, so it
// is tested on every platform; the reading itself comes from IStartupProbe::readImpact().

#include "Platform/AsciiCase.h"
#include "Platform/IStartupProbe.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>

namespace Platform
{

/// What one startup entry's processes cost at the measured sign-in.
struct StartupImpactTotals
{
    std::uint64_t cpuUs = 0;
    std::uint64_t diskBytes = 0;
    std::uint32_t processCount = 0; ///< Its program's processes and every process they started.
};

/// @p path in the form sameProgramPath() compares: without surrounding spaces and double quotes, and
/// without the "\\?\" (or NT "\??\") prefix a long path carries. A UNC path keeps one leading
/// separator, so "\\server\share\a.exe" and "\\?\UNC\server\share\a.exe" both give "\server\share\a.exe".
[[nodiscard]] constexpr std::string_view comparableProgramPath(std::string_view path) noexcept
{
    while (!path.empty() && (path.front() == ' ' || path.front() == '"'))
    {
        path.remove_prefix(1);
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '"'))
    {
        path.remove_suffix(1);
    }
    for (const std::string_view prefix : {std::string_view{R"(\\?\)"}, std::string_view{R"(\??\)"}})
    {
        if (path.starts_with(prefix))
        {
            path.remove_prefix(prefix.size());
            // A slice, not substr(): substr's out-of-range throw would escape this noexcept function.
            if (path.size() > 4 && equalsIgnoreCase(std::string_view{path.begin(), path.begin() + 4}, R"(UNC\)"))
            {
                path.remove_prefix(3); // "UNC\server\..." -> "\server\..."
            }
            return path;
        }
    }
    if (path.starts_with("\\\\") || path.starts_with("//"))
    {
        path.remove_prefix(1); // "\\server\..." -> "\server\..."
    }
    return path;
}

/// Whether @p a and @p b name the same program file, as Windows compares paths: ASCII letters
/// case-insensitively, '/' and '\' alike, each in comparableProgramPath()'s form.
/// Neither is resolved further: a short (8.3) name or a path through a link differs from the long one.
/// Empty paths match nothing.
[[nodiscard]] constexpr bool sameProgramPath(std::string_view a, std::string_view b) noexcept
{
    a = comparableProgramPath(a);
    b = comparableProgramPath(b);
    if (a.empty() || a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const char x = (a[i] == '/') ? '\\' : foldAscii(a[i]);
        const char y = (b[i] == '/') ? '\\' : foldAscii(b[i]);
        if (x != y)
        {
            return false;
        }
    }
    return true;
}

/// What the program at @p executablePath cost at the sign-in @p reading measured: every recorded
/// process of that program, and every process one of them started (and those started, and so on),
/// each counted once -- a browser's own child processes are part of what starting it costs. Zero
/// processes when the program didn't run during the measurement, or @p executablePath is empty.
/// @p reading's processes must be ordered by start time (a parent starts before its children), as
/// IStartupProbe::readImpact() gives them.
[[nodiscard]] inline StartupImpactTotals measureStartupProgram(const StartupImpactReading& reading, std::string_view executablePath)
{
    StartupImpactTotals totals;
    if (comparableProgramPath(executablePath).empty())
    {
        return totals;
    }
    std::set<std::pair<std::uint32_t, std::uint64_t>> counted; // (PID, start ticks) of each counted process
    for (const StartupImpactProcess& process : reading.processes)
    {
        if (!sameProgramPath(process.path, executablePath) && !counted.contains({process.parentPid, process.parentStartTicks}))
        {
            continue;
        }
        if (!counted.insert({process.pid, process.startTicks}).second)
        {
            continue; // recorded twice
        }
        totals.cpuUs += process.cpuUs;
        totals.diskBytes += process.diskBytes;
        ++totals.processCount;
    }
    return totals;
}

} // namespace Platform
