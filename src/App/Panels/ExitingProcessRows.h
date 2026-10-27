#pragma once

// The Processes table's rows for processes that just exited (#3060), as Process Explorer and System
// Informer keep an exited process on screen for a moment: a process gone between two refreshes is
// otherwise never seen. Each is a copy of its last snapshot, drawn dimmed and tinted, never selectable
// or acted on, and not counted. Pure, so tested without ImGui (test_ExitingProcessRows.cpp).

#include "Domain/ProcessSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace App::ExitingProcessRows
{

/// How long an exited process's row stays, in seconds: about two refreshes at the default interval.
inline constexpr double DURATION_SECONDS = 2.0;

/// The opacity of the exiting tint behind the row: steady, not animated.
inline constexpr float TINT_ALPHA = 0.22F;

/// The opacity the row's text and icons are drawn at, so it reads as gone.
inline constexpr float CONTENT_ALPHA = 0.55F;

/// An exited process: its last snapshot, and when it was seen gone.
struct Exited
{
    Domain::ProcessSnapshot snapshot;
    double exitedAt = 0.0;
};

/// The exited processes still shown, oldest first.
class Tracker
{
  public:
    /// A new generation, @p current, replaced @p previous at @p now: each process of @p previous not in
    /// @p current exited. Returns whether any was added.
    bool onGeneration(const std::vector<Domain::ProcessSnapshot>& previous, const std::vector<Domain::ProcessSnapshot>& current, double now)
    {
        std::unordered_set<std::uint64_t> live;
        live.reserve(current.size());
        for (const Domain::ProcessSnapshot& proc : current)
        {
            live.insert(proc.uniqueKey);
        }
        bool added = false;
        for (const Domain::ProcessSnapshot& proc : previous)
        {
            if (!live.contains(proc.uniqueKey))
            {
                Exited exited{.snapshot = proc, .exitedAt = now};
                exited.snapshot.childrenIndices.clear(); // indices into a vector that is gone
                m_Exited.push_back(std::move(exited));
                added = true;
            }
        }
        return added;
    }

    /// Drops the processes exited DURATION_SECONDS or more before @p now. Returns whether any was dropped.
    bool expire(double now)
    {
        return std::erase_if(m_Exited, [now](const Exited& exited) { return now - exited.exitedAt >= DURATION_SECONDS; }) > 0;
    }

    /// Drops them all (the setting turned off). Returns whether any was dropped.
    bool clear() noexcept
    {
        const bool any = !m_Exited.empty();
        m_Exited.clear();
        return any;
    }

    [[nodiscard]] const std::vector<Exited>& exited() const noexcept
    {
        return m_Exited;
    }

  private:
    std::vector<Exited> m_Exited;
};

/// The rows the table draws: @p current as it is, then each of @p exited, so every index into @p current
/// still means the same process and an index at or past current.size() is an exited one. An exited
/// process whose parent is still listed (its PID, started no later than it: running, else exited with
/// it) is placed among that parent's children, so the tree shows it where it was; any other is a root.
[[nodiscard]] inline std::vector<Domain::ProcessSnapshot> displayRows(const std::vector<Domain::ProcessSnapshot>& current,
                                                                      const std::vector<Exited>& exited)
{
    std::vector<Domain::ProcessSnapshot> rows;
    rows.reserve(current.size() + exited.size());
    rows = current;
    for (const Exited& gone : exited)
    {
        rows.push_back(gone.snapshot);
    }
    // A running process keeps its PID's entry: emplace() leaves the first one.
    std::unordered_map<std::int32_t, std::size_t> byPid;
    byPid.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        byPid.emplace(rows[i].pid, i);
    }
    for (std::size_t index = current.size(); index < rows.size(); ++index)
    {
        if (const auto parent = byPid.find(rows[index].parentPid);
            parent != byPid.end() && parent->second != index && rows[parent->second].startTimeTicks <= rows[index].startTimeTicks)
        {
            rows[parent->second].childrenIndices.push_back(index);
        }
    }
    return rows;
}

/// How many of @p indices are of running processes, not exited ones: those below @p liveCount.
[[nodiscard]] inline std::size_t countLive(const std::vector<std::size_t>& indices, std::size_t liveCount) noexcept
{
    std::size_t live = 0;
    for (const std::size_t index : indices)
    {
        live += index < liveCount ? 1U : 0U;
    }
    return live;
}

} // namespace App::ExitingProcessRows
