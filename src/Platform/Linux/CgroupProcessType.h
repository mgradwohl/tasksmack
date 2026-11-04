#pragma once

/// @file CgroupProcessType.h
/// @brief The Processes table's Type on Linux (#3430): App, Background Process or System Process, from what the probe
/// already reads: /proc/[pid]/cgroup and the kernel-thread flag in /proc/[pid]/stat. No read of its own.

#include <cstdint>
#include <string_view>

namespace Platform::CgroupProcessType
{

inline constexpr std::string_view APP = "App";
inline constexpr std::string_view BACKGROUND = "Background Process";
inline constexpr std::string_view SYSTEM = "System Process";

/// The kernel's PF_KTHREAD flag. Its own copy, not ProcParsing's, so this header (and its test, which builds on Windows
/// too) needs no POSIX headers; LinuxProcessProbe.cpp static_asserts the two agree.
inline constexpr std::uint32_t PF_KTHREAD = 0x00200000U;

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

/// Whether the kernel's PF_* @p flags (/proc/[pid]/stat field 9) mark a kernel thread. By the flag, not by PID: inside
/// a container's PID namespace PID 2 can be an ordinary process.
[[nodiscard]] constexpr bool isKernelThread(std::uint32_t flags) noexcept
{
    return (flags & PF_KTHREAD) != 0;
}

/// The Type of a process that isn't a kernel thread (the probe gives kernel threads System Process from their stat
/// flags, isKernelThread()), whose /proc/[pid]/cgroup reads @p cgroups. @p inContainer: the probe recognised a container
/// in those cgroups (CgroupContainer), which wins over the path: Docker's systemd driver runs containers in
/// `system.slice/docker-<id>.scope`, and their processes are Background, while the host's docker.service stays
/// System. Without a readable cgroup it's Background.
[[nodiscard]] constexpr std::string_view classify(std::string_view cgroups, bool inContainer)
{
    if (inContainer)
    {
        return BACKGROUND;
    }
    return fromCgroupPath(Detail::cgroupPath(cgroups));
}

} // namespace Platform::CgroupProcessType
