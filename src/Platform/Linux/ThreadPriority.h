#pragma once

// The pass bookkeeping behind LinuxProcessActions::setPriority (#1104), free of /proc and syscalls
// so its branches -- partial failure, exited workers, reused TIDs, threads still starting -- are
// unit-testable (#1228 review).

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace Platform
{

struct PriorityChange
{
    std::size_t changed = 0;
    std::size_t failed = 0;
    int firstError = 0;
    bool threadsKeptStarting = false; // new threads were still appearing after the last pass
    std::error_code relistError;      // a listing after the first failed (for any reason)
    std::size_t unconfirmed = 0;      // worker threads that left the process around the call (TID maybe reused)
};

/// Sets the priority of every thread of `pid`, through `listThreads(pid)` (its thread IDs, or why
/// they couldn't be listed), `setNice(tid)` (0, or the errno of setpriority(2)) and
/// `isThreadOf(pid, tid)` -- injectable so the bookkeeping can be tested deterministically.
///
/// A thread can start another between the listing and its own renice; the new one inherits the old
/// nice and isn't in that listing. So the threads are listed again after each pass until a pass
/// finds none it hasn't already set, giving up after a bounded number of passes (#1228 review).
/// A worker that exits meanwhile (ESRCH) is neither a change nor a failure, and its ID is forgotten
/// in case a new thread reuses it; the main thread's exit is a failure. The first listing's failure
/// is an error; a later one is returned in relistError for the caller to report after its pidfd
/// check.
template<typename ListThreads, typename SetNice, typename IsThreadOf>
[[nodiscard]] std::expected<PriorityChange, std::error_code>
reniceThreads(std::int32_t pid, ListThreads listThreads, SetNice setNice, IsThreadOf isThreadOf)
{
    constexpr int MAX_PASSES = 8;
    PriorityChange change;
    std::unordered_set<id_t> seen;
    for (int pass = 0; pass < MAX_PASSES; ++pass)
    {
        const auto tids = listThreads(pid);
        if (!tids.has_value())
        {
            if (pass == 0)
            {
                return std::unexpected(tids.error());
            }
            // Kept whatever it is: even ENOENT/ESRCH doesn't prove the process exited --
            // /proc/<pid>/task goes away when the leader calls pthread_exit while other threads
            // run on. The caller's pidfd check reports a real exit first; a live process with an
            // incomplete relist is reported as such (#1228 review).
            change.relistError = tids.error();
            return change;
        }
        bool foundNew = false;
        for (const id_t tid : *tids)
        {
            if (!seen.insert(tid).second)
            {
                continue;
            }
            foundNew = true;
            const int err = setNice(tid);
            if (err == 0)
            {
                // The leader is covered by the caller's pidfd check; a worker is checked here.
                if (std::cmp_not_equal(tid, pid) && !isThreadOf(pid, tid))
                {
                    ++change.unconfirmed;
                    continue;
                }
                ++change.changed;
                continue;
            }
            if (err == ESRCH && std::cmp_not_equal(tid, pid))
            {
                // Exited: forget the ID, so if a new thread of this process reuses it before the
                // next listing it is reniced rather than taken as already done (#1228 review).
                seen.erase(tid);
                continue;
            }
            ++change.failed;
            if (change.firstError == 0)
            {
                change.firstError = err;
            }
        }
        if (!foundNew)
        {
            return change;
        }
    }
    change.threadsKeptStarting = true;
    return change;
}

} // namespace Platform
