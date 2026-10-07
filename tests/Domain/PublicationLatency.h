#pragma once

/// @file PublicationLatency.h
/// @brief Shared harness for the #868 regression tests: does a model's publication() wait for its writer?
///
/// One thread publishes generations back to back while the calling thread reads publication() in a
/// loop, timing each call. Before #868 the writer built every generation (a copy of each history
/// ring) under the lock publication() takes, so a reader landing in that window waited for most of a
/// write: about one slow read per generation. With the build moved outside the lock, a read waits for
/// a pointer swap at most, and only scheduler noise makes one slow.
///
/// The two threads are paced, not left to the scheduler: the writer starts each write only once the
/// reader has completed a read since the previous write started. So the reader is running before the
/// first write, every write is matched by at least one read, and a loaded machine can't let the
/// writer finish before the reader gets a time slice (which would leave nothing measured).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace TestPublication
{

struct LatencyResult
{
    std::size_t writes = 0;
    std::size_t reads = 0;
    std::size_t slowReads = 0; ///< Reads longer than a quarter of the median write
    double medianWriteMs = 0.0;
    double maxReadMs = 0.0;
    std::size_t versionRegressions = 0;    ///< A read returning an older generation than an earlier read
    std::size_t versionAheadOfPointer = 0; ///< publicationVersion() ran ahead of the following publication()
    std::size_t inconsistentReads = 0;     ///< A generation that failed the caller's consistency check
    bool pacingTimedOut = false;           ///< The reader never caught up with a write; the writer stopped early
};

/// How long the writer waits for the reader's next read before giving up (see pacingTimedOut).
inline constexpr std::chrono::seconds PACING_TIMEOUT{10};

/// Runs `writes` calls of write() on a second thread, each one publishing one generation, while this
/// thread reads. load() is publication(), version() is publicationVersion(), and consistent(pub)
/// checks one generation's internal invariants (series lengths); only load() is timed. Unless
/// pacingTimedOut, reads > writes: each write waits for a fresh read, and one more follows the last.
template<typename Write, typename Load, typename Version, typename Consistent>
[[nodiscard]] LatencyResult measure(std::size_t writes, Write write, Load load, Version version, Consistent consistent)
{
    using Clock = std::chrono::steady_clock;
    const auto toMs = [](Clock::duration d)
    {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    LatencyResult result;
    result.writes = writes;
    std::vector<double> writeMs;
    writeMs.reserve(writes);
    std::vector<double> readMs;
    readMs.reserve(1U << 16U);

    std::atomic<std::size_t> completedReads{0};
    std::atomic<bool> timedOut{false};
    std::atomic<bool> done{false};
    std::thread writer(
        [&]
        {
            std::size_t readsAtLastWrite = 0;
            for (std::size_t i = 0; i < writes; ++i)
            {
                // Wait for a read completed since the previous write started (before the first write:
                // any read, so the reader is known to be running).
                const auto deadline = Clock::now() + PACING_TIMEOUT;
                while (completedReads.load(std::memory_order_acquire) <= readsAtLastWrite)
                {
                    if (Clock::now() >= deadline)
                    {
                        timedOut.store(true, std::memory_order_relaxed);
                        done.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
                readsAtLastWrite = completedReads.load(std::memory_order_acquire);
                const auto begin = Clock::now();
                write(i);
                writeMs.push_back(toMs(Clock::now() - begin));
            }
            done.store(true, std::memory_order_release);
        });

    std::uint64_t lastSeen = 0;
    const auto readOnce = [&]
    {
        const std::uint64_t announced = version();
        const auto begin = Clock::now();
        const auto publication = load();
        readMs.push_back(toMs(Clock::now() - begin));
        if (publication->version < lastSeen)
        {
            ++result.versionRegressions;
        }
        if (publication->version < announced)
        {
            ++result.versionAheadOfPointer;
        }
        lastSeen = std::max(lastSeen, publication->version);
        if (!consistent(*publication))
        {
            ++result.inconsistentReads;
        }
        completedReads.fetch_add(1, std::memory_order_release);
    };
    while (!done.load(std::memory_order_acquire))
    {
        readOnce();
    }
    readOnce(); // the last generation too
    writer.join();

    result.reads = readMs.size();
    result.pacingTimedOut = timedOut.load(std::memory_order_relaxed);
    if (!writeMs.empty())
    {
        std::vector<double> sorted = writeMs;
        std::ranges::nth_element(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2));
        result.medianWriteMs = sorted[sorted.size() / 2];
    }
    const double slowThresholdMs = result.medianWriteMs / 4.0;
    for (const double ms : readMs)
    {
        result.maxReadMs = std::max(result.maxReadMs, ms);
        if (ms > slowThresholdMs)
        {
            ++result.slowReads;
        }
    }
    return result;
}

} // namespace TestPublication
