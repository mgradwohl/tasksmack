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

/// For a tool TaskSmack runs as root (#3605): the canonical path @p path resolves to, when that is an executable file
/// only root could have put there (SyscallTrace::onlyRootCouldPlace() on every component); nullopt otherwise. Run the
/// path this returns, never @p path (#4396): @p path may be a link in a folder another account can write, which passes
/// the check by pointing at a root-owned tool and can then be pointed at that account's own script before each exec.
[[nodiscard]] inline std::optional<std::string> rootTrustedPath(const std::string& path)
{
    if (!isExecutableFile(path))
    {
        return std::nullopt;
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
    if (ec || !isExecutableFile(resolved.native()))
    {
        return std::nullopt;
    }
    const bool trusted = SyscallTrace::onlyRootCouldPlace(resolved.native(),
                                                          [](const std::string& entry) -> std::optional<SyscallTrace::PathOwnership>
                                                          {
                                                              struct stat info{};
                                                              if (::lstat(entry.c_str(), &info) != 0)
                                                              {
                                                                  return std::nullopt;
                                                              }
                                                              return SyscallTrace::PathOwnership{.uid = info.st_uid, .mode = info.st_mode};
                                                          });
    if (!trusted)
    {
        return std::nullopt;
    }
    return resolved.native();
}

/// Whether rootTrustedPath() accepts @p path.
[[nodiscard]] inline bool isRootTrustedExecutable(const std::string& path)
{
    return rootTrustedPath(path).has_value();
}

/// Where to run the tool @p name from: as root (@p root), only SyscallTrace::ROOT_TOOL_PATH's directories and only a file
/// root alone could have placed, answered as the canonical path that was checked (rootTrustedPath(), #4396); otherwise
/// @p pathEnv ($PATH) and any executable file, as a shell would.
[[nodiscard]] inline std::optional<std::string> find(std::string_view name, bool root, std::string_view pathEnv)
{
    if (root)
    {
        const std::optional<std::string> found = SyscallTrace::findExecutable(name, SyscallTrace::ROOT_TOOL_PATH, &isRootTrustedExecutable);
        return found.has_value() ? rootTrustedPath(*found) : std::nullopt;
    }
    return SyscallTrace::findExecutable(name, pathEnv, &isExecutableFile);
}

/// TaskSmack's environment @p env (NAME=value entries, null-terminated, as environ) for a program it starts as root and
/// as itself (#3855): the same variables, the display and session bus included, which a terminal or xdg-open needs, but
/// PATH set to SyscallTrace::ROOT_TOOL_PATH. gcore and xdg-open are scripts that run their own helpers by name; with the
/// invoking user's PATH (sudo -E, su without -) a helper of theirs would run as root.
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
    out.push_back(std::string("PATH=") + std::string(SyscallTrace::ROOT_TOOL_PATH));
    return out;
}

/// The environment a tool run as root gets in place of TaskSmack's own (#3760): only a PATH of the system's directories,
/// so nothing the invoking user set (PATH, LD_*, locale or tool variables) reaches it.
[[nodiscard]] inline std::vector<std::string> rootEnvironment()
{
    return {std::string("PATH=") + std::string(SyscallTrace::ROOT_TOOL_PATH)};
}

} // namespace Platform::RootTools
