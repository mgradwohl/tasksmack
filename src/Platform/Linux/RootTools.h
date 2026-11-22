#pragma once

/// @file RootTools.h
/// @brief Finding the programs TaskSmack runs, and running them safely as root (#3605, #3760). Run as root for a user
/// (sudo -E, or any preserved PATH), $PATH is theirs to point at a program of their own, which would then run as root:
/// so as root a tool is looked up only in SyscallTrace::ROOT_TOOL_PATH, and taken only if root alone could have put it
/// where it resolves to, and it gets an environment of TaskSmack's own, not the one it was started with.

#include "Platform/Linux/SyscallTrace.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace Platform::RootTools
{

/// Whether @p path is a regular file this process may execute.
[[nodiscard]] inline bool isExecutableFile(const std::string& path)
{
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

/// The filesystem calls the root checks make, injectable so a test can give them a tree with a group-writable
/// /usr/local (#4396). systemRootFs() is the real one.
struct RootFs
{
    /// lstat(): who owns @p path and its mode, nullopt when it can't be read.
    std::function<std::optional<SyscallTrace::PathOwnership>(const std::string&)> ownership;
    /// readlink(): where the link @p path points, nullopt when @p path isn't a link.
    std::function<std::optional<std::string>(const std::string&)> readLink;
    /// std::filesystem::canonical(), nullopt on failure.
    std::function<std::optional<std::string>(const std::string&)> canonical;
    /// isExecutableFile().
    std::function<bool(const std::string&)> executable;
    /// Whether the folder @p path is on procfs (statfs()), wherever it is mounted; empty: only /proc itself counts.
    /// The "= {}" lets a designated initializer leave it out without -Wmissing-designated-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::function<bool(const std::string&)> onProcfs = {};
    /// Whether @p path is on a filesystem whose owners and modes another account can make up (#4554):
    /// untrustedFilesystem() of its statfs(), and a statfs that fails. Empty: none is.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::function<bool(const std::string&)> onUntrustedFilesystem = {};
};

/// Whether a filesystem of statfs type @p type, mounted with statfs flags @p flags, could show owners and modes another
/// account chose (#4554, the #4572 review), so nothing on it is root-only whatever lstat() says:
/// - a user's mount: nosuid, which udisks (a user's own disk image, its files root:0755 as they made them),
///   fusermount and fstab "user" mounts all force, and a working /usr never is (sudo lives there);
/// - FUSE (sshfs, fuseblk, glusterfs) reports any owner it likes, root's included, when mounted allow_other;
/// - NFS, CIFS/SMB, CEPH, AFS and 9p carry owners and ACLs from elsewhere, which st_mode doesn't show.
/// OverlayFS and ecryptfs keep their lower filesystem's real owners, and are trusted.
[[nodiscard]] constexpr bool untrustedFilesystem(std::uint64_t type, std::uint64_t flags) noexcept
{
    constexpr std::uint64_t NOSUID = 2; // ST_NOSUID, <sys/statvfs.h>; statfs's f_flags uses the same bits
    // <linux/magic.h>: FUSE_SUPER_MAGIC, NFS_SUPER_MAGIC, CIFS_SUPER_MAGIC, SMB2_SUPER_MAGIC, CEPH_SUPER_MAGIC,
    // AFS_SUPER_MAGIC, AFS_FS_MAGIC (kAFS), V9FS_MAGIC.
    constexpr std::array<std::uint64_t, 8> UNTRUSTED{
        0x65735546U,
        0x6969U,
        0xFF534D42U,
        0xFE534D42U,
        0x00C36400U,
        0x5346414FU,
        0x6B414653U,
        0x01021997U,
    };
    return (flags & NOSUID) != 0 || std::ranges::contains(UNTRUSTED, type & 0xFFFFFFFFU);
}

[[nodiscard]] inline RootFs systemRootFs()
{
    return {
        .ownership = [](const std::string& path) -> std::optional<SyscallTrace::PathOwnership>
        {
            struct stat info{};
            if (::lstat(path.c_str(), &info) != 0)
            {
                return std::nullopt;
            }
            return SyscallTrace::PathOwnership{.uid = info.st_uid, .mode = info.st_mode};
        },
        .readLink = [](const std::string& path) -> std::optional<std::string>
        {
            std::error_code ec;
            if (!std::filesystem::is_symlink(path, ec))
            {
                return std::nullopt;
            }
            const std::filesystem::path target = std::filesystem::read_symlink(path, ec);
            return ec ? std::nullopt : std::optional<std::string>(target.native());
        },
        .canonical = [](const std::string& path) -> std::optional<std::string>
        {
            std::error_code ec;
            const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
            return ec ? std::nullopt : std::optional<std::string>(resolved.native());
        },
        .executable = &isExecutableFile,
        .onProcfs =
            [](const std::string& path)
        {
            struct statfs info{};
            constexpr decltype(info.f_type) PROC_FS = 0x9fa0; // PROC_SUPER_MAGIC, <linux/magic.h>
            return ::statfs(path.c_str(), &info) == 0 && info.f_type == PROC_FS;
        },
        .onUntrustedFilesystem =
            [](const std::string& path)
        {
            struct statfs info{};
            if (::statfs(path.c_str(), &info) != 0)
            {
                return true; // unknown: not trusted
            }
            return untrustedFilesystem(static_cast<std::uint64_t>(static_cast<std::uint32_t>(info.f_type)),
                                       static_cast<std::uint64_t>(info.f_flags));
        },
    };
}

/// The folder holding @p path ("/" for a path directly under it).
[[nodiscard]] inline std::string parentFolder(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return (slash == 0 || slash == std::string::npos) ? std::string("/") : path.substr(0, slash);
}

/// Whether @p path, or any folder above it, is on a filesystem RootFs::onUntrustedFilesystem refuses: statfs() of a
/// mount point reports what is mounted there, not what holds it, so a trusted filesystem mounted below a user's FUSE
/// folder would pass on its own (the #4572 review).
[[nodiscard]] inline bool onUntrustedMount(const std::string& path, const RootFs& fs)
{
    if (!fs.onUntrustedFilesystem)
    {
        return false;
    }
    if (fs.onUntrustedFilesystem(path))
    {
        return true;
    }
    for (std::size_t slash = path.rfind('/'); slash != std::string::npos && slash > 0; slash = path.rfind('/', slash - 1))
    {
        if (fs.onUntrustedFilesystem(path.substr(0, slash)))
        {
            return true;
        }
    }
    return path != "/" && fs.onUntrustedFilesystem("/");
}

/// The canonical path of the folder @p folder when only root could change what is in it: that path, and every folder
/// above it, is root's and writable by no group or other user (SyscallTrace::onlyRootCouldPlace()); nullopt otherwise.
[[nodiscard]] inline std::optional<std::string> rootOnlyFolder(const std::string& folder, const RootFs& fs)
{
    std::optional<std::string> resolved = fs.canonical(folder);
    if (!resolved.has_value() || !SyscallTrace::onlyRootCouldPlace(*resolved, fs.ownership) || onUntrustedMount(*resolved, fs))
    {
        return std::nullopt;
    }
    return resolved;
}

/// Whether rootOnlyFolder() accepts @p folder.
[[nodiscard]] inline bool isRootOnlyFolder(const std::string& folder, const RootFs& fs)
{
    return rootOnlyFolder(folder, fs).has_value();
}

/// Whether every link among the folders on the way to @p path (as written, each prefix /a, /a/b, ...) sits in a folder
/// only root could change (the #4551 review): a link there that another account can replace would choose where the
/// rest of the path leads, whatever folder it leads to.
[[nodiscard]] inline bool noForeignLinkOnTheWay(const std::string& path, const RootFs& fs)
{
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
    {
        const std::string prefix = path.substr(0, slash);
        if (fs.readLink(prefix).has_value() && !isRootOnlyFolder(parentFolder(prefix), fs))
        {
            return false;
        }
    }
    return true;
}

/// @p pathList (colon-separated) with only the folders isRootOnlyFolder() accepts, in order (#4396): what TaskSmack
/// searches for a tool as root, and the PATH it gives a program it runs as root. A /usr/local/bin another account can
/// write (Debian's root:staff 2775, or a chown -R of /usr/local) is left out, so nothing there is found or run by name.
/// Each folder is given as the canonical path that was checked, once (#4550): a folder named through a link (a
/// staff-writable /usr/local holding "bin -> /usr/bin") would pass on its target, and every child resolving the PATH
/// later would follow wherever the link had been re-pointed. On merged-usr, /bin and /usr/bin are one entry.
/// A trusted folder vouches for what is in it, not for where it points: a link root made in one (/usr/bin/foo ->
/// /usr/local/bin/foo) is followed by a child that runs foo by name, unchecked. Only the tools TaskSmack finds itself
/// (find()) have every hop checked.
[[nodiscard]] inline std::string trustedRootPath(std::string_view pathList, const RootFs& fs)
{
    std::string out;
    std::vector<std::string> listed;
    std::size_t start = 0;
    while (start <= pathList.size())
    {
        std::size_t end = pathList.find(':', start);
        if (end == std::string_view::npos)
        {
            end = pathList.size();
        }
        const std::string folder(pathList.substr(start, end - start));
        const std::optional<std::string> resolved = folder.starts_with('/') ? rootOnlyFolder(folder, fs) : std::nullopt;
        if (resolved.has_value() && !std::ranges::contains(listed, *resolved))
        {
            out += out.empty() ? *resolved : ":" + *resolved;
            listed.push_back(*resolved);
        }
        start = end + 1;
    }
    return out;
}

/// What a root child's PATH holds when no folder is trusted (the #4551 review): a folder in /proc, which nothing can
/// create, so no name is found. Never an empty PATH, which glibc's execvp, dash and bash all read as "search the
/// current directory", and never no PATH, which they read as /bin:/usr/bin, the folders that just failed the check.
inline constexpr std::string_view NO_TRUSTED_FOLDER_PATH = "/proc/nonexistent";

/// The "PATH=..." a root child gets for the trusted folders @p trusted (trustedRootPath()).
[[nodiscard]] inline std::string rootPathVariable(const std::string& trusted)
{
    return "PATH=" + (trusted.empty() ? std::string(NO_TRUSTED_FOLDER_PATH) : trusted);
}

/// trustedRootPath() of SyscallTrace::ROOT_TOOL_PATH on this machine.
[[nodiscard]] inline std::string trustedRootPath()
{
    return trustedRootPath(SyscallTrace::ROOT_TOOL_PATH, systemRootFs());
}

/// Whether @p path names each folder plainly: absolute, with no "." or ".." component and no empty one ("//").
[[nodiscard]] inline bool isPlainAbsolutePath(std::string_view path)
{
    if (!path.starts_with('/') || path.ends_with('/'))
    {
        return false;
    }
    for (std::size_t start = 1; start <= path.size();)
    {
        std::size_t end = path.find('/', start);
        end = end == std::string_view::npos ? path.size() : end;
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
        {
            return false;
        }
        start = end + 1;
    }
    return true;
}

/// For a tool TaskSmack runs as root (#3605): the path to run for @p path, when it leads to an executable file only root
/// could have put there; nullopt otherwise. Every link on the way, a folder's or a hop's, must sit in a folder only root
/// could change (#4396, #4551): a link another account can replace would choose which root-owned program runs
/// (xdg-open -> python3, which runs a folder's __main__.py), and could re-point it before the exec. So only root can
/// change where @p path leads. @p path itself is what runs (#4531) when every hop's folder is named as it is, with no
/// link in it: a multi-call program dispatches on its argv[0], so pkill run as /usr/bin/pgrep was pgrep. Otherwise the
/// canonical path is answered (the #4564 review): a link nested inside a folder link's target (a root link to /opt/app,
/// itself a link into a staff-writable folder) is checked only where it leads, not where it sits, and a /proc magic
/// link changes with the process behind it, so the kernel's walk of such a path could be changed after the check.
/// Run the path this returns.
[[nodiscard]] inline std::optional<std::string> rootTrustedPath(const std::string& path, const RootFs& fs)
{
    if (!path.starts_with('/') || !fs.executable(path))
    {
        return std::nullopt;
    }
    constexpr int MAX_LINK_HOPS = 40; // the kernel's own limit on a path's links
    std::string hop = path;
    bool namedAsItIs = isPlainAbsolutePath(path); // every hop's folder is its own canonical path: no folder link walked
    for (int hops = 0;; ++hops)
    {
        const std::string folder = parentFolder(hop);
        const std::optional<std::string> folderResolved = rootOnlyFolder(folder, fs);
        if (hops > MAX_LINK_HOPS || !folderResolved.has_value() || !noForeignLinkOnTheWay(hop, fs))
        {
            return std::nullopt;
        }
        // A /proc magic link (/proc/<pid>/exe, .../fd/<n>) passes every root-only check, but leads to whatever the
        // process holding that PID at the exec has (the #4564 re-review): never run by such a name.
        const bool procfs = folder == "/proc" || folder.starts_with("/proc/") || (fs.onProcfs && fs.onProcfs(folder));
        namedAsItIs = namedAsItIs && *folderResolved == folder && !procfs;
        const std::optional<std::string> target = fs.readLink(hop);
        if (!target.has_value())
        {
            break;
        }
        hop = target->starts_with('/') ? *target : parentFolder(hop) + "/" + *target;
    }
    std::optional<std::string> resolved = fs.canonical(path);
    if (!resolved.has_value() || !fs.executable(*resolved) || !SyscallTrace::onlyRootCouldPlace(*resolved, fs.ownership) ||
        onUntrustedMount(*resolved, fs))
    {
        return std::nullopt;
    }
    return namedAsItIs ? std::optional<std::string>(path) : resolved;
}

[[nodiscard]] inline std::optional<std::string> rootTrustedPath(const std::string& path)
{
    return rootTrustedPath(path, systemRootFs());
}

/// For a library loaded into a root or otherwise privileged process (#4668): rootTrustedPath()'s checks, for a regular file
/// rather than an executable one. Every folder on the way only root can change, every link too, the file root's and no
/// one else's to write, and not on an untrusted mount, so no other account chose the code the process runs.
/// @return The path to dlopen(), as rootTrustedPath() answers it; nullopt when another account could have put it there.
[[nodiscard]] inline std::optional<std::string> rootTrustedLibrary(const std::string& path, const RootFs& fs)
{
    // A regular file where the path leads, by the same calls the other checks make (a test's tree, or lstat()).
    const RootFs asLibrary{
        .ownership = fs.ownership,
        .readLink = fs.readLink,
        .canonical = fs.canonical,
        .executable =
            [&fs](const std::string& file)
        {
            const std::optional<std::string> resolved = fs.canonical(file);
            const std::optional<SyscallTrace::PathOwnership> entry = resolved.has_value() ? fs.ownership(*resolved) : std::nullopt;
            return entry.has_value() && (entry->mode & S_IFMT) == S_IFREG;
        },
        .onProcfs = fs.onProcfs,
        .onUntrustedFilesystem = fs.onUntrustedFilesystem,
    };
    return rootTrustedPath(path, asLibrary);
}

[[nodiscard]] inline std::optional<std::string> rootTrustedLibrary(const std::string& path)
{
    return rootTrustedLibrary(path, systemRootFs());
}

/// Whether rootTrustedPath() accepts @p path.
[[nodiscard]] inline bool isRootTrustedExecutable(const std::string& path)
{
    return rootTrustedPath(path).has_value();
}

/// The tool @p name as root, searched for in trustedRootPath(@p pathList) only, and answered as the path rootTrustedPath()
/// checked (#4396, #4531), checked once.
[[nodiscard]] inline std::optional<std::string> findRootTool(std::string_view name, std::string_view pathList, const RootFs& fs)
{
    std::optional<std::string> verified;
    const std::optional<std::string> found = SyscallTrace::findExecutable(name,
                                                                          trustedRootPath(pathList, fs),
                                                                          [&](const std::string& candidate)
                                                                          {
                                                                              verified = rootTrustedPath(candidate, fs);
                                                                              return verified.has_value();
                                                                          });
    return found.has_value() ? verified : std::nullopt;
}

/// Where to run the tool @p name from: as root (@p root), findRootTool() over SyscallTrace::ROOT_TOOL_PATH; otherwise
/// @p pathEnv ($PATH) and any executable file, as a shell would.
[[nodiscard]] inline std::optional<std::string> find(std::string_view name, bool root, std::string_view pathEnv)
{
    if (root)
    {
        return findRootTool(name, SyscallTrace::ROOT_TOOL_PATH, systemRootFs());
    }
    return SyscallTrace::findExecutable(name, pathEnv, &isExecutableFile);
}

/// TaskSmack's environment @p env (NAME=value entries, null-terminated, as environ) for a program it starts as root and
/// as itself (#3855): the same variables, the display and session bus included, which a terminal or xdg-open needs, but
/// PATH set to trustedRootPath(), ROOT_TOOL_PATH's root-only folders (#4396). gcore and xdg-open are scripts that run their own helpers by
/// name; with the invoking user's PATH (sudo -E, su without -) a helper of theirs would run as root.
[[nodiscard]] inline std::vector<std::string> withRootPath(const char* const* env)
{
    std::vector<std::string> out;
    for (const char* const* entry = env; entry != nullptr && *entry != nullptr; ++entry)
    {
        const std::string_view variable(*entry);
        if (!variable.starts_with("PATH="))
        {
            out.emplace_back(variable);
        }
    }
    out.push_back(rootPathVariable(trustedRootPath())); // root-only folders only (#4396): a script's helpers are found by name
    return out;
}

/// The environment a tool run as root gets in place of TaskSmack's own (#3760): only a PATH of the system's root-only directories
/// (trustedRootPath()), so nothing the invoking user set (PATH, LD_*, locale or tool variables) reaches it.
[[nodiscard]] inline std::vector<std::string> rootEnvironment()
{
    return {rootPathVariable(trustedRootPath())}; // root-only folders only (#4396): gcore runs "#!/usr/bin/env bash"
}

} // namespace Platform::RootTools
