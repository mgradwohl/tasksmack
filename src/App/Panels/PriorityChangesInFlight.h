#pragma once

// Which processes have a priority change asked about or running, and in which panel (#3467). The
// Processes table (F7/F8, Set Realtime priority..., Set priority for N processes...) and Process Details
// (the priority slider's Apply and its retry, Set Realtime...) each run their changes on a worker of
// their own, so without this, one started in each panel for the same process both ran, and whichever
// finished last decided the class: possibly the older one, while the other's result line said it had
// worked. Now each panel publishes the processes it has a change in flight for, and holds its own
// priority controls for a process another panel has published, saying why.
//
// Data only, on the UI thread only: ShellPanels owns one and hands it to both panels, which publish
// from their update and render paths (the workers' results still reach them through their own
// ProcessActionRun / BatchRun polls, as before) and read it while drawing. No lock, no thread.
// A process is its PID and start time (Platform::ProcessTarget's ==), so a reused PID is never held.

#include "Platform/IProcessActions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace App
{

class PriorityChangesInFlight
{
  public:
    /// The panels that change a process's priority.
    enum class Panel : std::uint8_t
    {
        Processes,      ///< The Processes table: F7/F8, Set Realtime priority..., Set priority for N processes...
        ProcessDetails, ///< Process Details: the priority slider's Apply and its retry, Set Realtime...
    };

    /// Replaces what @p panel has in flight with @p targets: a change asked about (its confirm or dialog
    /// up) or running on the panel's worker, its result not yet taken in.
    void publish(Panel panel, std::span<const Platform::ProcessTarget> targets)
    {
        claims(panel).assign(targets.begin(), targets.end());
    }

    /// Whether a panel other than @p asker has a priority change for @p target in flight: the same PID
    /// and the same start time. @p asker's own are its own business (#3429 holds them within a panel).
    [[nodiscard]] bool heldElsewhere(Panel asker, const Platform::ProcessTarget& target) const noexcept
    {
        std::size_t panel = 0;
        for (const std::vector<Platform::ProcessTarget>& claimed : m_Claims)
        {
            if (panel++ != static_cast<std::size_t>(asker) && std::ranges::find(claimed, target) != claimed.end())
            {
                return true;
            }
        }
        return false;
    }

    /// heldElsewhere()'s reason, for @p asker's disabled controls' tooltips, or null when @p target isn't held.
    [[nodiscard]] const char* holdReason(Panel asker, const Platform::ProcessTarget& target) const noexcept
    {
        return heldElsewhere(asker, target) ? waitReason(asker) : nullptr;
    }

    /// Why @p asker's priority controls for a held process wait: they name the panel the change runs in.
    [[nodiscard]] static constexpr const char* waitReason(Panel asker) noexcept
    {
        return asker == Panel::Processes ? "Waiting for the priority change in Process Details to finish"
                                         : "Waiting for the priority change in the Processes tab to finish";
    }

    /// What @p asker says when it refuses a change for a held process (F7/F8, a confirm accepted anyway).
    [[nodiscard]] static constexpr const char* refusal(Panel asker) noexcept
    {
        return asker == Panel::Processes ? "A priority change for this process is still running in Process Details"
                                         : "A priority change for this process is still running in the Processes tab";
    }

  private:
    static constexpr std::size_t PANEL_COUNT = 2;

    [[nodiscard]] std::vector<Platform::ProcessTarget>& claims(Panel panel)
    {
        return m_Claims.at(static_cast<std::size_t>(panel));
    }

    std::array<std::vector<Platform::ProcessTarget>, PANEL_COUNT> m_Claims;
};

} // namespace App
