#pragma once

/// @file PublicationLatency.h
/// @brief Shared harness for the #868 regression tests: does a model's publication() wait for its writer?
///
/// One thread publishes generations back to back while the calling thread reads publication() in a
/// loop, timing each call. Before #868 the writer built every generation (a copy of each history
/// ring) under the lock publication() takes, so a reader landing in that window waited for most of a
/// write: about one slow read per generation. With the build moved outside the lock, a read waits for
/// a pointer swap at most, and only scheduler noise makes one slow.

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
};

/// Runs `writes` calls of write() on a second thread, each one publishing one generation, while this
/// thread reads. load() is publication(), version() is publicationVersion(), and consistent(pub)
/// checks one generation's internal invariants (series lengths); only load() is timed.
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

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::thread writer(
        [&]
        {
            started.store(true, std::memory_order_release);
            for (std::size_t i = 0; i < writes; ++i)
            {
                const auto begin = Clock::now();
                write(i);
                writeMs.push_back(toMs(Clock::now() - begin));
            }
            done.store(true, std::memory_order_release);
        });

    while (!started.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

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
    };
    while (!done.load(std::memory_order_acquire))
    {
        readOnce();
    }
    readOnce(); // the last generation too
    writer.join();

    result.reads = readMs.size();
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
