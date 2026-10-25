#pragma once

// The rule shared by Process Details' priority edits -- the nice value / priority class
// (ProcessPriorityView) and the Linux I/O priority (ProcessIoPriorityView, #803) -- for whether an
// edit made for one process may still be applied to the process the panel shows now.

#include "Platform/IProcessActions.h"
#include "ProcessActionsView.h"

namespace App::Detail
{

/// Whether a priority edit made for @p edited may still be applied to @p live. As
/// isSameProcessTarget(), except that an edit made before the process's start time was known (0) does
/// not carry over once @p live knows it: the PID may have been reused before the first snapshot, so
/// the edit could be for a different process, and the platform's own check of @p live cannot tell.
/// The user edits again once the identity is known. While both stay unknown the edit is kept but
/// cannot be applied: every IProcessActions refuses a target whose start time is 0, so
/// ProcessPriorityView::canApply() is false until the start time is known.
[[nodiscard]] constexpr bool isSameEditTarget(const Platform::ProcessTarget& edited, const Platform::ProcessTarget& live) noexcept
{
    if (edited.startTimeTicks == 0 && live.startTimeTicks != 0)
    {
        return false;
    }
    return isSameProcessTarget(edited, live);
}

} // namespace App::Detail
