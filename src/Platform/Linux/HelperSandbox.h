#pragma once

// tasksmack-helper's seccomp allow-list (#2752), installed after HelperPrivileges has dropped root. A bug in
// one of the parsers the helper runs on other users' data (environ, maps, fd links, journal fields) could
// run code with the helper's four capabilities; the filter takes away what that code would want and the
// helper never does: writing or creating files, exec, loading code (no PROT_EXEC mappings), ptrace and
// process_vm_*, sockets other than its broker socket (fd 0), open_by_handle_at, mounts, namespaces, BPF,
// io_uring, keyrings and changing its IDs or capabilities. Its own job stays allowed: reading /proc, the
// journal, coredumps and firmware files, pidfd_send_signal, setpriority, ioprio_set and sched_setaffinity.
//
// What it can't take away is the helper's job itself: with CAP_SYS_PTRACE a read-only open of
// /proc/<pid>/mem still reads another process's memory, and the renice, I/O priority and affinity calls
// stay open; the user-space checks guard those.
//
// A named refusal fails the call with an errno (so glibc falls back from openat2, and a late dlopen fails
// cleanly); anything not listed is logged (Mode::Log) or kills the helper (Mode::Enforce). The program is
// raw BPF over the build architecture's <sys/syscall.h> numbers, x86-64 or AArch64; any other
// architecture or ABI (x32) is killed.

#include "Platform/Linux/HelperSandboxMode.h"

#include <vector>

#include <linux/filter.h>
#include <sys/types.h>

namespace Platform::HelperSandbox
{

/// Whether this build has a filter for its architecture (x86-64 or AArch64).
[[nodiscard]] bool supported() noexcept;

/// The BPF program installFilter() loads, for @p mode, for the process @p self (the only one it may
/// signal by number); empty when !supported(). For tests and review.
[[nodiscard]] std::vector<sock_filter> program(Mode mode, pid_t self);

/// Loads what the helper needs after the filter goes in, while it can still load code: libsystemd's
/// decompressors (zstd, xz, lz4), which it dlopen()s the first time a journal field needs one. A library
/// that isn't installed is skipped. Call before installFilter().
void preloadLibraries() noexcept;

/// Installs program(@p mode) on this process (one thread: call before starting any). Needs no_new_privs,
/// which HelperPrivileges::dropToKeptCapabilities() sets. @return false when it couldn't (the helper
/// must not serve) or when !supported().
[[nodiscard]] bool installFilter(Mode mode) noexcept;

} // namespace Platform::HelperSandbox
