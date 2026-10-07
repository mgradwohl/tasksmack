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
///
/// The pacing guarantees reads, not contention: strict alternation (read, whole write, read, ...)
/// would satisfy it with no read ever landing inside a write, and then even a writer holding the
/// reader's lock for its whole copy would show no slow read. So the writer raises an "in write" flag
/// around each write, the reader notes whether each read started while it was up (an overlapping
/// read), and a trial only counts once it has MIN_OVERLAPPING_READS of them. A trial short of that is
/// rerun, up to MAX_TRIALS; overlapAchieved says whether one succeeded, and only then is slowReads
/// meaningful.

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
    std::size_t overlappingReads = 0;      ///< Reads that started while a write was in progress
    std::size_t slowOverlappingReads = 0;  ///< Of those, the slow ones
    std::size_t trials = 0;                ///< Trials run (see MAX_TRIALS); the other fields describe the last
    std::size_t totalWrites = 0;           ///< Writes over all trials, i.e. generations published
    bool overlapAchieved = false;          ///< The last trial had at least MIN_OVERLAPPING_READS
};

/// Reads that must land inside a write for a trial to measure contention.
inline constexpr std::size_t MIN_OVERLAPPING_READS = 10;
/// Trials to run before giving up on reaching MIN_OVERLAPPING_READS.
inline constexpr std::size_t MAX_TRIALS = 5;

/// How long the writer waits for the reader's next read before giving up (see pacingTimedOut).
inline constexpr std::chrono::seconds PACING_TIMEOUT{10};

/// Paces writer threads on a reader's progress, so reads really happen while publishing is in
/// progress however the scheduler runs the threads: each writer waits, before each publish, for a
/// read completed since its previous publish. The reader calls readDone() after every read. Waits are
/// bounded by PACING_TIMEOUT; a timeout is recorded (timedOut()) and the writer should stop.
class ReadPacer
{
  public:
    void readDone() noexcept
    {
        m_Reads.fetch_add(1, std::memory_order_release);
    }

    /// Wait for a read newer than `lastSeen` (start at 0: the reader is known to be running), then
    /// advance `lastSeen` to it. False on timeout.
    [[nodiscard]] bool awaitReadSince(std::size_t& lastSeen)
    {
        const auto deadline = std::chrono::steady_clock::now() + PACING_TIMEOUT;
        while (m_Reads.load(std::memory_order_acquire) <= lastSeen)
        {
            if (m_TimedOut.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline)
            {
                m_TimedOut.store(true, std::memory_order_relaxed);
                return false;
            }
            std::this_thread::yield();
        }
        lastSeen = m_Reads.load(std::memory_order_acquire);
        return true;
    }

    [[nodiscard]] bool timedOut() const noexcept
    {
        return m_TimedOut.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::size_t> m_Reads{0};
    std::atomic<bool> m_TimedOut{false};
};

/// One trial of measure(): `writes` calls of write(firstIndex + i) on a second thread while this
/// thread reads.
template<typename Write, typename Load, typename Version, typename Consistent>
[[nodiscard]] LatencyResult
runTrial(std::size_t writes, std::size_t firstIndex, Write& write, Load& load, Version& version, Consistent& consistent)
{
    using Clock = std::chrono::steady_clock;
    const auto toMs = [](Clock::duration d)
    {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    LatencyResult result;
    std::vector<double> writeMs;
    writeMs.reserve(writes);
    std::vector<double> readMs;
    readMs.reserve(1U << 16U);
    std::vector<bool> readOverlapped;
    readOverlapped.reserve(1U << 16U);

    std::atomic<std::size_t> completedReads{0};
    std::atomic<bool> timedOut{false};
    std::atomic<bool> done{false};
    std::atomic<bool> inWrite{false};
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
                inWrite.store(true, std::memory_order_seq_cst);
                const auto begin = Clock::now();
                write(firstIndex + i);
                writeMs.push_back(toMs(Clock::now() - begin));
                inWrite.store(false, std::memory_order_seq_cst);
            }
            done.store(true, std::memory_order_release);
        });

    std::uint64_t lastSeen = 0;
    const auto readOnce = [&]
    {
        const std::uint64_t announced = version();
        const bool overlapping = inWrite.load(std::memory_order_seq_cst);
        const auto begin = Clock::now();
        const auto publication = load();
        readMs.push_back(toMs(Clock::now() - begin));
        readOverlapped.push_back(overlapping);
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

    result.writes = writeMs.size();
    result.reads = readMs.size();
    result.pacingTimedOut = timedOut.load(std::memory_order_relaxed);
    if (!writeMs.empty())
    {
        std::vector<double> sorted = writeMs;
        std::ranges::nth_element(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2));
        result.medianWriteMs = sorted[sorted.size() / 2];
    }
    const double slowThresholdMs = result.medianWriteMs / 4.0;
    for (std::size_t i = 0; i < readMs.size(); ++i)
    {
        const double ms = readMs[i];
        result.maxReadMs = std::max(result.maxReadMs, ms);
        const bool slow = ms > slowThresholdMs;
        if (slow)
        {
            ++result.slowReads;
        }
        if (readOverlapped[i])
        {
            ++result.overlappingReads;
            if (slow)
            {
                ++result.slowOverlappingReads;
            }
        }
    }
    return result;
}

/// Runs trials of `writes` calls of write() on a second thread, each one publishing one generation,
/// while this thread reads, until a trial has MIN_OVERLAPPING_READS reads that started inside a write
/// (at most MAX_TRIALS). write(i) gets a running index over all trials. load() is publication(),
/// version() is publicationVersion(), and consistent(pub) checks one generation's internal invariants
/// (series lengths); only load() is timed. Unless pacingTimedOut, a trial has reads > writes: each
/// write waits for a fresh read, and one more follows the last.
template<typename Write, typename Load, typename Version, typename Consistent>
[[nodiscard]] LatencyResult measure(std::size_t writes, Write write, Load load, Version version, Consistent consistent)
{
    LatencyResult result;
    std::size_t totalWrites = 0;
    for (std::size_t trial = 1; trial <= MAX_TRIALS; ++trial)
    {
        result = runTrial(writes, totalWrites, write, load, version, consistent);
        totalWrites += result.writes;
        result.trials = trial;
        result.totalWrites = totalWrites;
        result.overlapAchieved = result.overlappingReads >= MIN_OVERLAPPING_READS;
        if (result.overlapAchieved || result.pacingTimedOut || result.versionRegressions != 0 || result.versionAheadOfPointer != 0 ||
            result.inconsistentReads != 0)
        {
            break; // done, or failed in a way another trial can't fix
        }
    }
    return result;
}

} // namespace TestPublication
