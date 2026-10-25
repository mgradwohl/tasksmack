#pragma once

// The systemd service a process runs under (#1886), from the cgroup path /proc/[pid]/cgroup names. systemd
// puts each unit in a cgroup named after it: a service's processes under ".../sshd.service", a user
// service's under ".../user@1000.service/app.slice/foo.service", a session's or an app's under a ".scope".
// The deepest unit in the path is the process's own: a ".scope" (a login session, an app launched from the
// desktop) means it is in no service. A ".service" is named by the outermost unit, the one the system
// manager runs, which is what the Services tab lists (#2458): a user service by its user manager
// (user@1000.service), a systemd container's units by the container's (systemd-nspawn@deb.service), and a
// unit inside a container scope (docker-<id>.scope/.../ssh.service) by none, never by a host unit that only
// shares its name. Pure, so it is unit-tested on fixture contents; LinuxProcessProbe reads the file once a
// pass for the container and freeze state already.

#include <cstddef>
#include <string>
#include <string_view>

namespace Platform::CgroupService
{

namespace Detail
{

/// The service of one cgroup path ("/system.slice/sshd.service"), or empty.
[[nodiscard]] inline std::string serviceFromPath(std::string_view path)
{
    std::string_view outermostUnit;
    std::string_view deepestUnit;
    std::size_t start = 0;
    while (start <= path.size())
    {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos)
        {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part.ends_with(".service") || part.ends_with(".scope"))
        {
            if (outermostUnit.empty())
            {
                outermostUnit = part;
            }
            deepestUnit = part;
        }
        start = end + 1;
    }
    if (!deepestUnit.ends_with(".service") || !outermostUnit.ends_with(".service") ||
        outermostUnit.size() == std::string_view(".service").size())
    {
        return {};
    }
    return std::string(outermostUnit);
}

} // namespace Detail

/// The service named by @p cgroups, /proc/[pid]/cgroup's contents: from the v2 ("0::") line, else the v1
/// line of systemd's own hierarchy ("name=systemd"). Empty for a process in no service: a session or app
/// scope, a kernel thread (the root cgroup), or a cgroup namespace that hides the path ("0::/").
[[nodiscard]] inline std::string serviceFromCgroups(std::string_view cgroups)
{
    std::string fromV1;
    std::size_t lineStart = 0;
    while (lineStart < cgroups.size())
    {
        std::size_t lineEnd = cgroups.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = cgroups.size();
        }
        const std::string_view line = cgroups.substr(lineStart, lineEnd - lineStart);
        if (line.starts_with("0::"))
        {
            return Detail::serviceFromPath(line.substr(3));
        }
        constexpr std::string_view SYSTEMD_V1 = ":name=systemd:";
        if (const std::size_t at = line.find(SYSTEMD_V1); fromV1.empty() && at != std::string_view::npos)
        {
            fromV1 = Detail::serviceFromPath(line.substr(at + SYSTEMD_V1.size()));
        }
        lineStart = lineEnd + 1;
    }
    return fromV1;
}

} // namespace Platform::CgroupService
