#pragma once

// Starting a program that outlives the call and is never waited for: the terminal "Trace system
// calls (strace)" opens (#182). See spawnDetached().

#include <cstdint>
#include <expected>
#include <span>
#include <string>

// NOLINTNEXTLINE(misc-include-cleaner) - pollfd and nfds_t come from <sys/poll.h>
#include <sys/poll.h>

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
/// they need (argv pointers, environment, /dev/null, the list of open descriptors) is prepared before
/// it.
///
/// The program inherits only stdin/stdout/stderr, all on /dev/null, even when TaskSmack itself was
/// started with those closed (the descriptors this uses are kept above them). Every other descriptor
/// is marked close-on-exec first: with close_range(2) where the kernel has it (5.11+), else one
/// fcntl(2) per descriptor that was open when the call began (from /proc/self/fd), else every
/// descriptor up to RLIMIT_NOFILE. The program's signal mask is cleared and every signal disposition
/// reset to default.
///
/// Whether execve() succeeded is reported back through a close-on-exec pipe, waited on for at most
/// two seconds (an exec does not normally take more than milliseconds). After that -- or if poll(2)
/// itself fails -- the program is assumed to have started: it may yet exec, so reporting a failure
/// could have the user start a second one.
[[nodiscard]] std::expected<void, SpawnFailure> spawnDetached(std::span<const std::string> argv);

namespace Detail
{

/// Test seams for spawnDetached(); production code uses the defaults.
struct SpawnHooks
{
    bool useCloseRange = true;                   ///< false exercises the per-descriptor fallback
    int (*poll)(pollfd*, nfds_t, int) = nullptr; ///< nullptr: ::poll
};

/// spawnDetached() with @p hooks.
[[nodiscard]] std::expected<void, SpawnFailure> spawnDetached(std::span<const std::string> argv, const SpawnHooks& hooks);

} // namespace Detail

} // namespace Platform::DetachedSpawn
