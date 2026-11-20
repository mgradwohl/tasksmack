#pragma once

// Where Run new task finds the program it was typed (#4532, #4400): looked up as the account it will run as would
// find it. Run as root, directly or through pkexec, a bare name is found only in the root-only system folders, as every
// other program TaskSmack runs as root is (RootTools::find(), #3605, #4396): a group-writable /usr/local/bin on
// TaskSmack's PATH, a root session started with a custom PATH, or a folder of the user's that pkexec would search, would
// otherwise supply what root runs. Run as the invoking user, in their session, it is their PATH, which TaskSmack's
// (root's) isn't, and only a file they may run. An absolute name is the path typed, run as typed.

#include "Platform/Linux/RootTools.h"
#include "Platform/Linux/SyscallTrace.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace Platform::RunProgramLookup
{

/// Whom the program will run as.
enum class RunsAs : std::uint8_t
{
    Self,         ///< As TaskSmack itself, not root and not asked for administrator
    Root,         ///< As root: Run as administrator (directly as root, or through pkexec), or root with no invoking user
    InvokingUser, ///< As the user TaskSmack runs as root for, in their session
};

/// Whom the program runs as: TaskSmack is @p root or not, @p asAdministrator was asked for, and @p hasUserSession says
/// whether root runs for an invoking user it can start programs as (SessionUser::invokingUserSession()).
[[nodiscard]] constexpr RunsAs runsAsFor(bool root, bool asAdministrator, bool hasUserSession) noexcept
{
    if (asAdministrator)
    {
        return RunsAs::Root; // as root itself, or through pkexec: either way root runs it
    }
    if (!root)
    {
        return RunsAs::Self;
    }
    return hasUserSession ? RunsAs::InvokingUser : RunsAs::Root;
}

/// Whether the user @p uid, of group @p gid and supplementary @p groups, may run the file @p path, judged from its mode
/// bits as the kernel judges them for that user (an owner's bits, else a group's, else other's), not as root, which may
/// run any file with an execute bit (the #4532 review): their shell would skip a file they can't run and search on.
[[nodiscard]] inline bool executableBy(const std::string& path, uid_t uid, gid_t gid, const std::vector<gid_t>& groups)
{
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
    {
        return false;
    }
    if (info.st_uid == uid)
    {
        return (info.st_mode & S_IXUSR) != 0;
    }
    if (info.st_gid == gid || std::ranges::contains(groups, info.st_gid))
    {
        return (info.st_mode & S_IXGRP) != 0;
    }
    return (info.st_mode & S_IXOTH) != 0;
}

/// The program @p typed names, as the account it runs as would find it: nullopt when it isn't found. @p ownPath is
/// TaskSmack's PATH, @p userPath the invoking user's (their session's environment), and @p userCanRun whether they may
/// run a file (executableBy() in the app). @p rootFind looks a bare name up as root (RootTools::find(name, true, ...) in
/// the app). Both are injectable for tests.
template<typename RootFind, typename UserCanRun>
[[nodiscard]] std::optional<std::string> findProgram(std::string_view typed,
                                                     RunsAs runsAs,
                                                     std::string_view ownPath,
                                                     std::string_view userPath,
                                                     const RootFind& rootFind,
                                                     const UserCanRun& userCanRun)
{
    if (runsAs == RunsAs::Root && !typed.contains('/'))
    {
        return rootFind(typed);
    }
    if (runsAs == RunsAs::InvokingUser)
    {
        return SyscallTrace::findExecutable(typed, userPath, userCanRun);
    }
    return SyscallTrace::findExecutable(typed, ownPath, &RootTools::isExecutableFile);
}

/// The PATH in @p environment ("NAME=value" entries), empty when it has none.
template<typename Entries> [[nodiscard]] std::string_view pathIn(const Entries& environment)
{
    for (const auto& entry : environment)
    {
        const std::string_view variable(entry);
        if (variable.starts_with("PATH="))
        {
            return variable.substr(5);
        }
    }
    return {};
}

} // namespace Platform::RunProgramLookup
