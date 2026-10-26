/// @file ProcessTreeFixture.h
/// @brief The synthetic process forest bench_ProcessTree.cpp builds its tree-view rows from, shared with
/// tests/Benchmarks/test_ProcessTreeFixture.cpp so a change that flattens the forest fails a test (#2976).
///
/// Before #2976 each process's parent was `(i * 2'654'435'761U) % i`: a multiple of `i` taken mod `i`,
/// so always 0, and every fixture was one root with every other process as its child. The benchmarks
/// timed a one-level star, not a tree.
///
/// The shape now, from integer math only (no std::hash, no floating point, no RNG distribution), so it is
/// the same on every platform and standard library:
///   - Processes 0-3 are roots (no parent), like Idle/System and the session roots whose parent exited.
///   - Every later process i takes the parent (a * b) / i, where a and b are the low and high 32 bits of
///     splitmix64(i), each mod i. That product of two uniform picks in [0, i) favors early processes, so
///     the early ones become hosts with many children (a service host, a shell) and the late ones leaves.
///
/// The result at the benchmarked sizes (BM_ProcessTree_BuildRows_* use 500, 2000 and 10000):
///   count   roots  processes with children  depth 1-6   max depth  mean depth  most children
///     500       4                      173       99 %           6         2.5             34
///    2000       4                      683       99 %           8         3.2             49
///   10000       4                     3463       96 %          10         4.0             58
/// so most processes sit 2-6 levels deep, as under a real system's services and shells, with a thin tail.

#pragma once

#include "Domain/ProcessSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace BenchmarkUtils
{

/// Processes with no parent at the front of every forest from makeProcessForest().
inline constexpr std::size_t PROCESS_FOREST_ROOTS = 4;

/// splitmix64's finalizer (Steele, Lea and Flood): a full-avalanche 64-bit mix, fixed-width on every platform.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t value) noexcept
{
    std::uint64_t mixed = value + 0x9E37'79B9'7F4A'7C15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return mixed ^ (mixed >> 31U);
}

/// The parent of process @p index (>= PROCESS_FOREST_ROOTS): an earlier process, in [0, index).
[[nodiscard]] constexpr std::size_t processForestParent(std::size_t index) noexcept
{
    const auto bound = static_cast<std::uint64_t>(index);
    const std::uint64_t hash = splitmix64(bound);
    const std::uint64_t first = (hash & 0xFFFF'FFFFULL) % bound;
    const std::uint64_t second = (hash >> 32U) % bound;
    // Each is below index (< 2^32 for any fixture), so the product fits and the quotient is below index.
    return static_cast<std::size_t>((first * second) / bound);
}

/// @p count processes in the forest described at the top of this file. Each has a pid and uniqueKey
/// of its index + 1, a cpuPercent for the sorted benchmark to order by, and its children's indices
/// listed in increasing order; nothing else is filled in.
[[nodiscard]] inline std::vector<Domain::ProcessSnapshot> makeProcessForest(std::size_t count)
{
    std::vector<Domain::ProcessSnapshot> snapshots(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        snapshots[i].pid = static_cast<std::int32_t>(i + 1);
        snapshots[i].uniqueKey = i + 1;
        snapshots[i].cpuPercent = static_cast<double>((i * 37U) % 101U);
    }
    for (std::size_t i = PROCESS_FOREST_ROOTS; i < count; ++i)
    {
        snapshots[processForestParent(i)].childrenIndices.push_back(i);
    }
    return snapshots;
}

} // namespace BenchmarkUtils
