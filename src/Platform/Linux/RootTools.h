#pragma once

/// @file RootTools.h
/// @brief Finding the programs TaskSmack runs, and running them safely as root (#3605, #3760). Run as root for a user
/// (sudo -E, or any preserved PATH), $PATH is theirs to point at a program of their own, which would then run as root:
/// so as root a tool is looked up only in SyscallTrace::ROOT_TOOL_PATH, and taken only if root alone could have put it
/// where it resolves to, and it gets an environment of TaskSmack's own, not the one it was started with.

#include "Platform/Linux/SyscallTrace.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace Platform::RootTools
{

/// Whether @p path is a regular file this process may execute.
[[nodiscard]] inline bool isExecutableFile(const std::string& path)
{
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

/// As isExecutableFile(), for a tool TaskSmack runs as root (#3605): also one only root could have put where it
/// resolves to (SyscallTrace::onlyRootCouldPlace()).
[[nodiscard]] inline bool isRootTrustedExecutable(const std::string& path)
{
    if (!isExecutableFile(path))
    {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
    if (ec)
    {
        return false;
    }
    return SyscallTrace::onlyRootCouldPlace(resolved.native(),
                                            [](const std::string& entry) -> std::optional<SyscallTrace::PathOwnership>
                                            {
                                                struct stat info{};
                                                if (::lstat(entry.c_str(), &info) != 0)
                                                {
                                                    return std::nullopt;
                                                }
                                                return SyscallTrace::PathOwnership{.uid = info.st_uid, .mode = info.st_mode};
                                            });
}

/// Where to run the tool @p name from: as root (@p root), only SyscallTrace::ROOT_TOOL_PATH's directories and only a file
/// root alone could have placed; otherwise @p pathEnv ($PATH) and any executable file, as a shell would.
[[nodiscard]] inline std::optional<std::string> find(std::string_view name, bool root, std::string_view pathEnv)
{
    if (root)
    {
        return SyscallTrace::findExecutable(name, SyscallTrace::ROOT_TOOL_PATH, &isRootTrustedExecutable);
    }
    return SyscallTrace::findExecutable(name, pathEnv, &isExecutableFile);
}

/// The environment a tool run as root gets in place of TaskSmack's own (#3760): only a PATH of the system's directories,
/// so nothing the invoking user set (PATH, LD_*, locale or tool variables) reaches it.
[[nodiscard]] inline std::vector<std::string> rootEnvironment()
{
    return {std::string("PATH=") + std::string(SyscallTrace::ROOT_TOOL_PATH)};
}

} // namespace Platform::RootTools
