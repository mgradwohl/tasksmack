#pragma once

#include "CgroupFreezerPath.h"
#include "ProcParsing.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Platform::CgroupPath
{

namespace Detail
{

/// Reads the start of a small cgroup control file; empty if it can't be read.
[[nodiscard]] inline std::string readSmallCgroupFile(const std::filesystem::path& path)
{
    std::array<char, 256> buf{};
    const std::string pathStr = path.string();
    const std::size_t len = ProcParsing::readProcFile(pathStr.c_str(), buf.data(), buf.size());
    return {buf.data(), len};
}

/// cgroup v2: `<root>/<path>/cgroup.events` holds "frozen 1" once the cgroup -- or an ancestor,
/// which freezes its descendants -- is frozen (systemctl freeze, docker pause, ...).
[[nodiscard]] inline bool isV2CgroupFrozen(const std::filesystem::path& cgroupRoot, std::string_view cgroupSubPath)
{
    const std::optional<std::filesystem::path> eventsPath = buildContainedCgroupPath(cgroupRoot, cgroupSubPath, "cgroup.events");
    if (!eventsPath.has_value())
    {
        return false;
    }
    const std::string events = readSmallCgroupFile(*eventsPath);
    return events.starts_with("frozen 1") || events.contains("\nfrozen 1");
}

/// cgroup v1: `<root>/freezer/<path>/freezer.state` is FROZEN, or FREEZING on the way there.
[[nodiscard]] inline bool isV1CgroupFrozen(const std::filesystem::path& cgroupRoot, std::string_view cgroupSubPath)
{
    // cgroupSubPath came out of /proc/<pid>/cgroup, so it is untrusted input to a file-access
    // function (CodeQL cpp/path-injection); buildContainedCgroupPath refuses anything that would
    // leave the hierarchy, which would make the prefix test below a content oracle.
    const std::optional<std::filesystem::path> statePath = buildContainedCgroupPath(cgroupRoot / "freezer", cgroupSubPath, "freezer.state");
    if (!statePath.has_value())
    {
        return false;
    }
    const std::string state = readSmallCgroupFile(*statePath);
    return state.starts_with("FROZEN") || state.starts_with("FREEZING");
}

} // namespace Detail

/// Whether the process whose /proc/<pid>/cgroup reads `procCgroupContents` is in a frozen cgroup,
/// under the cgroup filesystem mounted at `cgroupRoot` (normally /sys/fs/cgroup; injectable for tests).
///
/// Each line is "hierarchy-ID:controllers:path". The cgroup v2 line is "0::<path>", and its freeze
/// state is in that cgroup's cgroup.events. A v1 line naming the freezer controller points into
/// the freezer hierarchy. Handling only v1 left a v2-only system -- Ubuntu 24.04 and most current
/// distributions -- never reporting Suspended (#1105).
[[nodiscard]] inline bool isCgroupFrozen(std::string_view procCgroupContents, const std::filesystem::path& cgroupRoot)
{
    while (!procCgroupContents.empty())
    {
        const std::size_t lineEnd = procCgroupContents.find('\n');
        const std::string_view line = procCgroupContents.substr(0, lineEnd);
        procCgroupContents = (lineEnd == std::string_view::npos) ? std::string_view{} : procCgroupContents.substr(lineEnd + 1);

        const std::size_t firstColon = line.find(':');
        const std::size_t secondColon = (firstColon == std::string_view::npos) ? std::string_view::npos : line.find(':', firstColon + 1);
        if (secondColon == std::string_view::npos)
        {
            continue;
        }
        const std::string_view hierarchyId = line.substr(0, firstColon);
        const std::string_view controllers = line.substr(firstColon + 1, secondColon - firstColon - 1);
        const std::string_view cgroupPath = line.substr(secondColon + 1);
        // The kernel always writes an absolute path; anything else isn't a cgroup line we understand.
        if (!cgroupPath.starts_with('/'))
        {
            continue;
        }
        const std::string_view relativePath = cgroupPath.substr(1);

        if (hierarchyId == "0" && controllers.empty())
        {
            if (Detail::isV2CgroupFrozen(cgroupRoot, relativePath))
            {
                return true;
            }
        }
        else if (controllers.contains("freezer"))
        {
            if (Detail::isV1CgroupFrozen(cgroupRoot, relativePath))
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace Platform::CgroupPath
