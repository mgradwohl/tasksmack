#pragma once

/// @file CgroupProcessType.h
/// @brief The Processes table's Type on Linux (#3430): App, Background Process or System Process, from the cgroup path
/// the probe already reads (/proc/[pid]/cgroup) and the parent PID it already parses, with no read of its own.

#include <cstdint>
#include <string_view>

namespace Platform::CgroupProcessType
{

inline constexpr std::string_view APP = "App";
inline constexpr std::string_view BACKGROUND = "Background Process";
inline constexpr std::string_view SYSTEM = "System Process";

namespace Detail
{

/// The process's cgroup path from /proc/[pid]/cgroup's contents: the v2 ("0::") line's, else the v1 line of
/// systemd's own hierarchy ("name=systemd"). Empty when neither is there.
[[nodiscard]] constexpr std::string_view cgroupPath(std::string_view cgroups)
{
    std::string_view fromV1;
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
            return line.substr(3);
        }
        constexpr std::string_view SYSTEMD_V1 = ":name=systemd:";
        if (const std::size_t at = line.find(SYSTEMD_V1); fromV1.empty() && at != std::string_view::npos)
        {
            fromV1 = line.substr(at + SYSTEMD_V1.size());
        }
        lineStart = lineEnd + 1;
    }
    return fromV1;
}

/// Whether @p unit is a desktop app's scope: "app-gnome-firefox-1234.scope", "app-flatpak-org.gimp.GIMP-5678.scope"
/// (the desktop or launcher starts each app in one, xdg's convention), or a snap app's "snap.firefox.firefox-<id>.scope".
[[nodiscard]] constexpr bool isAppScope(std::string_view unit) noexcept
{
    return unit.ends_with(".scope") && (unit.starts_with("app-") || unit.starts_with("snap."));
}

} // namespace Detail

/// The Type of the process in @p path (a cgroup path, "/user.slice/user-1000.slice/...").
/// - System Process: the system's own services and init (`system.slice`, `init.scope`).
/// - App: a desktop app's scope anywhere under the user's manager (`app-*.scope`, a snap's `snap.*.scope`).
/// - Background Process: everything else: the user's own services (`user@UID.service`), a session's processes that
///   aren't an app (a terminal's shell, `session-N.scope`), a container's, or a path a cgroup namespace hides ("/").
[[nodiscard]] constexpr std::string_view fromCgroupPath(std::string_view path)
{
    if (path.starts_with("/system.slice") || path.starts_with("/init.scope"))
    {
        return SYSTEM;
    }
    std::size_t start = 0;
    while (start < path.size())
    {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos)
        {
            end = path.size();
        }
        if (Detail::isAppScope(path.substr(start, end - start)))
        {
            return APP;
        }
        start = end + 1;
    }
    return BACKGROUND;
}

/// The Type for a process with @p pid and parent @p parentPid whose /proc/[pid]/cgroup reads @p cgroups. Kernel
/// threads are System: kthreadd (PID 2) and its children. Without a readable cgroup, anything else is Background.
[[nodiscard]] constexpr std::string_view classify(std::string_view cgroups, std::int32_t pid, std::int32_t parentPid)
{
    if (pid == 2 || parentPid == 2)
    {
        return SYSTEM;
    }
    return fromCgroupPath(Detail::cgroupPath(cgroups));
}

} // namespace Platform::CgroupProcessType
