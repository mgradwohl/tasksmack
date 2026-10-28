#pragma once

// The Processes table's tint for a process stuck in uninterruptible sleep (#3097), as htop highlights D
// state: a process waiting on a hung NFS mount or a failing disk can't be killed and is easy to miss.
// Processes pass through D for a moment on most disk reads, so only one that stays in D for
// STUCK_SECONDS is tinted. Decided once per generation, so drawing needs no clock. Pure, so tested
// without ImGui (test_StuckProcesses.cpp).

#include "Domain/ProcessSnapshot.h"
#include "Domain/ProcessState.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App::StuckProcesses
{

/// How long a process must stay in D before its row is tinted, in seconds.
inline constexpr double STUCK_SECONDS = 2.0;

/// The tint's opacity behind a stuck process's row, in the theme's disk-sleep colour: steady, not animated.
inline constexpr float TINT_ALPHA = 0.20F;

/// Which processes have been in D since when, and which of them count as stuck.
class Tracker
{
  public:
    /// A new generation, @p current, read at @p now (seconds on a steady clock). A process in D keeps
    /// the time it entered D; one that left D, or is gone, is forgotten.
    void onGeneration(const std::vector<Domain::ProcessSnapshot>& current, double now)
    {
        std::unordered_map<std::uint64_t, double> since;
        m_Stuck.clear();
        for (const Domain::ProcessSnapshot& proc : current)
        {
            if (Domain::processStateCode(proc.displayState) != 'D')
            {
                continue;
            }
            const auto found = m_Since.find(proc.uniqueKey);
            const double entered = found != m_Since.end() ? found->second : now;
            since.emplace(proc.uniqueKey, entered);
            if (now - entered >= STUCK_SECONDS)
            {
                m_Stuck.insert(proc.uniqueKey);
            }
        }
        m_Since = std::move(since);
    }

    /// Whether the process keyed @p uniqueKey (ProcessSnapshot::uniqueKey) had been in D for STUCK_SECONDS
    /// at the last generation.
    [[nodiscard]] bool stuck(std::uint64_t uniqueKey) const
    {
        return m_Stuck.contains(uniqueKey);
    }

  private:
    std::unordered_map<std::uint64_t, double> m_Since;
    std::unordered_set<std::uint64_t> m_Stuck;
};

} // namespace App::StuckProcesses
