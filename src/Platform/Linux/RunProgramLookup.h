#pragma once

// Where Run new task finds the program it was typed (#4532, #4400): looked up as the account it will run as would
// find it. Run as root, a bare name is found only in the root-only system folders, as every other program TaskSmack
// runs as root is (RootTools::find(), #3605, #4396): under sudo -E TaskSmack's PATH is the invoking user's, and a
// folder of theirs on it would supply what root runs. Run as the invoking user, in their session, it is their own PATH,
// which TaskSmack's (root's) isn't. A name with a "/" is the path typed, as a shell takes it.

#include "Platform/Linux/RootTools.h"
#include "Platform/Linux/SyscallTrace.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Platform::RunProgramLookup
{

/// Whom the program will run as.
enum class RunsAs : std::uint8_t
{
    Self,         ///< As TaskSmack itself, not root (pkexec in front of it is root's choice to make, after a password)
    Root,         ///< As root: TaskSmack is root and runs it as itself (Run as administrator, or no invoking user)
    InvokingUser, ///< As the user TaskSmack runs as root for, in their session
};

/// The program @p typed names, as the account it runs as would find it: nullopt when it isn't found. @p ownPath is
/// TaskSmack's PATH, @p userPath the invoking user's (their session's environment). @p rootFind looks a bare name up as
/// root (RootTools::find(name, true, ...) in the app), injectable for tests.
template<typename RootFind>
[[nodiscard]] std::optional<std::string>
findProgram(std::string_view typed, RunsAs runsAs, std::string_view ownPath, std::string_view userPath, const RootFind& rootFind)
{
    if (runsAs == RunsAs::Root && !typed.contains('/'))
    {
        return rootFind(typed);
    }
    return SyscallTrace::findExecutable(typed, runsAs == RunsAs::InvokingUser ? userPath : ownPath, &RootTools::isExecutableFile);
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
