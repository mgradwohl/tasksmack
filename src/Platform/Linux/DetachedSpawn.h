#pragma once

// Starting a program that outlives the call and is never waited for: the terminal "Trace system
// calls (strace)" opens (#182). See spawnDetached().

#include <cstdint>
#include <expected>
#include <span>
#include <string>

// NOLINTBEGIN(misc-include-cleaner) - pollfd and nfds_t come from <sys/poll.h>, pid_t from <sys/types.h>
#include <sys/poll.h>
#include <sys/types.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::DetachedSpawn
{

/// Why spawnDetached() failed, and at which step.
struct SpawnFailure
{
    enum class Stage : std::uint8_t
    {
        Setup, ///< A bad argv, no pipe or /dev/null descriptor, or setsid() failed in the intermediate child.
        Fork,  ///< fork() failed (in TaskSmack or in the intermediate child), or (ETIMEDOUT) the
               ///< intermediate child did not exit within the time limit and was killed.
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
/// handler is needed. If setsid() fails in that child, it reports the error and exits, and the call
/// fails. After fork() the children call only async-signal-safe functions; everything they need (argv
/// pointers, environment, /dev/null, signal actions) is prepared before it.
///
/// The program inherits only stdin/stdout/stderr, all on /dev/null, even when TaskSmack itself was
/// started with those closed (the descriptors this uses are kept above them). Every other descriptor
/// is marked close-on-exec first, in the program's own (single-threaded) process just before execve(),
/// so a descriptor another TaskSmack thread opens meanwhile cannot slip through: with close_range(2)
/// where the kernel has it (5.11+), else one fcntl(2) per descriptor the child lists in its own
/// /proc/self/fd (with the raw getdents64(2) syscall, no allocation), else every descriptor up to
/// RLIMIT_NOFILE. The program's signal mask is cleared and every signal disposition
/// reset to default.
///
/// Whether execve() succeeded is reported back through a close-on-exec pipe. The whole call -- that
/// wait and reaping the intermediate child -- is bounded by one two-second deadline (an exec does not
/// normally take more than milliseconds). If the report has not come by then -- or if poll(2) itself
/// fails -- the program is assumed to have started: it may yet exec, so reporting a failure could have
/// the user start a second one. An intermediate child still running at the deadline is killed and
/// reaped, and the call fails with ETIMEDOUT.
[[nodiscard]] std::expected<void, SpawnFailure> spawnDetached(std::span<const std::string> argv);

namespace Detail
{

/// Test seams for spawnDetached(); production code uses the defaults.
struct SpawnHooks
{
    bool useCloseRange = true;                   ///< false exercises the per-descriptor fallback
    int (*poll)(pollfd*, nfds_t, int) = nullptr; ///< nullptr: ::poll
    bool useProcFdList = true;                   ///< false (with useCloseRange false): the RLIMIT_NOFILE walk
    pid_t (*setsid)() = nullptr;                 ///< In the intermediate child; async-signal-safe. nullptr: ::setsid
    void (*beforeFork)() = nullptr;              ///< Called just before fork(), after all setup
};

/// spawnDetached() with @p hooks.
[[nodiscard]] std::expected<void, SpawnFailure> spawnDetached(std::span<const std::string> argv, const SpawnHooks& hooks);

} // namespace Detail

} // namespace Platform::DetachedSpawn
