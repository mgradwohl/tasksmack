#pragma once

// Starting a program that outlives the call and is never waited for: the terminal "Trace system
// calls (strace)" opens (#182). See spawnDetached().

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace Platform::DetachedSpawn
{

/// Why spawnDetached() failed, and at which step.
struct SpawnFailure
{
    enum class Stage : std::uint8_t
    {
        Setup, ///< Before forking: a bad argv, or no pipe or /dev/null descriptor.
        Fork,  ///< fork() failed (in TaskSmack or in the intermediate child).
        Exec,  ///< execve() of argv[0] failed in the detached child.
    };

    Stage stage = Stage::Setup;
    int error = 0; ///< errno.
};

/// A user-facing message for @p failure starting @p program ("Could not start /usr/bin/xterm: ...").
[[nodiscard]] std::string spawnFailureMessage(const SpawnFailure& failure, const std::string& program);

/// Run @p argv (argv[0] an absolute path, exec'd directly: no PATH search and no shell) detached from
/// TaskSmack, and return once it has been exec'd -- never waiting for it to finish.
///
/// Double fork: TaskSmack's child starts a new session (setsid), forks the program's process and exits
/// at once; TaskSmack reaps that child immediately, and the program, orphaned, is adopted by init (or
/// the nearest subreaper), which reaps it when it exits. No zombie is left behind and no SIGCHLD
/// handler is needed. After fork() the children call only async-signal-safe functions; everything
/// they need (argv pointers, environment, /dev/null) is prepared before it.
///
/// The program inherits only stdin/stdout/stderr, all on /dev/null: every other descriptor is marked
/// close-on-exec first (close_range, or one fcntl per descriptor on kernels before 5.11). Its signal
/// mask is cleared and every signal disposition reset to default.
///
/// Whether execve() succeeded is reported back through a close-on-exec pipe, waited on for at most
/// a short timeout (an exec does not normally take more than milliseconds); after that the program is
/// assumed to have started.
[[nodiscard]] std::expected<void, SpawnFailure> spawnDetached(std::span<const std::string> argv);

} // namespace Platform::DetachedSpawn
