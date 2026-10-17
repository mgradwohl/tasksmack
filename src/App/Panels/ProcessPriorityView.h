#pragma once

// Process Details' priority control, under the buttons of the Overview's Actions block (#1179, slice 4;
// #1493): the nice-value slider on Linux, the same slider with a stop per priority class on Windows
// (#1204, #1538), the Apply
// button, and the error line under it; and under those, on Linux, the I/O priority control (#803,
// ProcessIoPriorityView).
//
// The view owns only its UI state. The IProcessActions it applies through stays owned by the panel (the
// composition root's Platform::makeProcessActions() result) and is passed in each frame, with the
// capabilities, the process's current nice value and the target, so this class never creates a probe.
//
// Everything but render() is defined here, free of ImGui, so the edit and apply state can be tested
// against a mock IProcessActions without an ImGui context (test_ProcessPriorityView.cpp).
//
// Apply runs on a worker of its own (ProcessActionRun, #2305), so a password prompt or a privileged
// helper's round trip never holds the frame: render() takes the result in on a later frame through
// update(). The panel declares the IProcessActions it passes in before this view, so the view (and its
// worker, waited for) is destroyed first.

#include "App/Panels/ProcessActionRun.h"
#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "PriorityEditTarget.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessIoPriorityView.h"

#include <cstdint>
#include <format>
#include <optional>
#include <stop_token>
#include <string>

namespace App
{

namespace Detail
{

/// @p current moved by @p delta steps (negative is a higher priority), held to the nice range: the
/// slider's arrow and page keys.
[[nodiscard]] constexpr std::int32_t stepNice(std::int32_t current, std::int32_t delta) noexcept
{
    return Domain::Priority::clampNice(current + delta);
}

/// What renderPriorityPicker() drew this frame.
struct PriorityPick
{
    std::int32_t nice = 0;  ///< The value picked this frame, or the one shown when nothing was picked
    float rightEdge = 0.0F; ///< Where the control ends, from the cursor's x, for right-aligning a button under it
};

/// Draws the priority picker showing @p shown, without a header: the gradient nice slider and its
/// keyboard shortcuts, or on Windows the same slider with a stop per settable priority class
/// (WINDOWS_PRIORITY_SLIDER, #1204, #1538). Shared by Process Details' control and the Processes
/// table's batch priority dialog (#1484), so both pick a priority the same way.
[[nodiscard]] PriorityPick renderPriorityPicker(std::int32_t shown);

/// What renderDiscretePrioritySlider() drew this frame.
struct DiscretePick
{
    std::int32_t index = 0;   ///< The stop picked this frame, or @p shown when nothing was picked
    float rightEdge = 0.0F;   ///< Where the control ends, from the cursor's x, for right-aligning a button under it
    bool labelsShown = false; ///< Whether the stops' scale labels were drawn (fitDiscreteTrack())
};

/// Draws @p slider's discrete mode (#1538) showing stop @p shown (PRIORITY_STOP_BEYOND_START for its
/// beyondStart state), as wide as the content region allows up to the slider's authored width, or up to
/// discretePrioritySliderMinWidth() where that is wider (fitDiscreteTrack(), #1984): the
/// stop's name on the badge, the shared gradient through the stops' colours, a tick and a scale label
/// under each stop (the labels only when the track has room for them all), and the thumb, hollow in
/// the beyond-start state. A drag snaps to the nearest stop; while the track is focused Left/Up step one
/// stop toward the high end, Right/Down one toward the low end, and Home/End jump to the ends
/// (stepDiscreteStop()).
/// @p inherited draws the thumb hollow at a stop the value was not set to but follows (the I/O slider's
/// default, derived from nice, #1540); picking any stop makes it explicit. A divider is drawn across the
/// track before each of @p slider's bandStarts.
[[nodiscard]] DiscretePick renderDiscretePrioritySlider(const DiscretePrioritySlider& slider, std::int32_t shown, bool inherited = false);

/// The narrowest width renderDiscretePrioritySlider() needs for @p slider's scale labels to show, at
/// the current font: the beyond-start lead and discreteStopLabelsMinWidth(). For measuring a container;
/// size it in whole pixels up from this (ImGui truncates a window's size), or the labels can miss by
/// the fraction lost (#1984).
[[nodiscard]] float discretePrioritySliderMinWidth(const DiscretePrioritySlider& slider);

} // namespace Detail

/// The priority control for the process Process Details shows.
///
/// An edit remembers the process it was made for. Apply acts only on that process: it is refused, and
/// the edit dropped, when the live target is a different process by then. A selection change drops the
/// edit and the error line, so an edited value can never be applied to another process.
class ProcessPriorityView
{
  public:
    /// Draws the control when @p capabilities allow setting priority, and the I/O priority control under
    /// it when they allow setting that (Linux), and nothing when they allow neither.
    /// @p currentNice is the process's nice value from its latest snapshot, or nullopt before the first
    /// snapshot (the control then shows 0 and Apply stays disabled). Apply also stays disabled while
    /// @p target's start time is unknown (0), which no platform will act on. An edit is made for @p target, and
    /// Apply sets it on @p target through @p actions (null gives an "unavailable" error).
    void render(Platform::IProcessActions* actions,
                const Platform::ProcessActionCapabilities& capabilities,
                std::optional<std::int32_t> currentNice,
                const Platform::ProcessTarget& target);

    /// Whether render() labels its rows: only when it draws two, the priority and the I/O priority
    /// (Linux), which the labels tell apart. With the priority row alone (Windows) the Priority card's
    /// header is its label, and the caller puts setCurrentPriorityTooltip() on that header (#1874).
    [[nodiscard]] static constexpr bool labelsRows(const Platform::ProcessActionCapabilities& capabilities) noexcept
    {
        return capabilities.canSetPriority && capabilities.canSetIoPriority;
    }

    /// Gives the last item drawn the process's current priority as its tooltip: the priority class on
    /// Windows, the nice value elsewhere (from @p currentNice, 0 before the first snapshot). It is the
    /// "Priority" row label's tooltip, or the card header's when the rows are unlabelled (labelsRows()).
    /// Needs an ImGui frame.
    static void setCurrentPriorityTooltip(std::optional<std::int32_t> currentNice);

    /// A different process was selected: drop the edit, its target and the error line, and the I/O
    /// priority control's.
    void onSelectionChanged() noexcept
    {
        ++m_Generation; // an apply still running reports to nobody: its process is no longer shown
        m_Retry.reset();
        m_NiceValue = Domain::Priority::NORMAL_NICE;
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        m_Error.clear();
        m_IoPriorityView.onSelectionChanged();
    }

    /// While nothing is edited, the control follows the process's own nice value (@p currentNice, when
    /// a snapshot has one).
    void syncToProcess(std::optional<std::int32_t> currentNice) noexcept
    {
        if (!m_Changed && currentNice.has_value())
        {
            m_NiceValue = *currentNice;
        }
    }

    /// The user picked @p nice (held to the nice range) for @p target. A value other than the shown one
    /// becomes a pending edit for @p target and clears the error line, for fresher feedback than a stale
    /// error; the same value changes nothing.
    void editNice(std::int32_t nice, const Platform::ProcessTarget& target)
    {
        const std::int32_t clamped = Domain::Priority::clampNice(nice);
        if (clamped == m_NiceValue)
        {
            return;
        }
        m_NiceValue = clamped;
        m_Changed = true;
        m_EditTarget = target;
        m_Error.clear();
        m_Retry.reset(); // a new edit replaces the refused one
    }

    /// Drops a pending edit that may be for a process other than @p liveTarget, which the panel can
    /// show without a selection change (the selected PID's process replaced), including an edit made
    /// before the start time was known once @p liveTarget knows it (Detail::isSameEditTarget()).
    /// Returns whether it did.
    bool dropEditIfTargetMoved(const Platform::ProcessTarget& liveTarget) noexcept
    {
        if (!m_Changed || Detail::isSameEditTarget(m_EditTarget, liveTarget))
        {
            return false;
        }
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        return true;
    }

    /// Whether Apply is enabled for @p liveTarget: there is a pending edit, a snapshot of the process,
    /// both the edit's target and @p liveTarget know the start time, and they are the same process
    /// (Detail::isSameEditTarget()). An action on a start time of 0 is refused by every IProcessActions
    /// (checkProcessIdentity()), so it is not offered.
    [[nodiscard]] bool canApply(std::optional<std::int32_t> currentNice, const Platform::ProcessTarget& liveTarget) const noexcept
    {
        // Self-contained: an edit for another process is never applicable, whether or not the caller
        // has dropped it with dropEditIfTargetMoved() first.
        return m_Changed && currentNice.has_value() && m_EditTarget.startTimeTicks != 0 && liveTarget.startTimeTicks != 0 &&
               Detail::isSameEditTarget(m_EditTarget, liveTarget);
    }

    /// Whether there is a pending edit that Apply cannot send yet for want of process details (a
    /// snapshot, or the start time that confirms the process's identity): Apply's tooltip then says so.
    [[nodiscard]] bool waitingForProcessDetails(std::optional<std::int32_t> currentNice,
                                                const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_Changed && !canApply(currentNice, liveTarget);
    }

    /// Apply was pressed with @p liveTarget selected: set the edited value on that target through
    /// @p actions, captured now and checked against the process the edit was made for, on the apply
    /// worker (#2305); update() takes the result in on a later frame. Success clears the error line;
    /// failure shows the platform's message and, unless the value has been edited again meanwhile, puts
    /// the control back at @p currentNice. Either way the edit is finished, so a second apply does
    /// nothing until the next edit. With no pending edit, one made for another process (dropped), a
    /// start time not yet known (kept, but not applicable: canApply()), or an apply still running (kept),
    /// there is no platform call. Null @p actions is the "unavailable" error at once.
    void apply(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget, std::optional<std::int32_t> currentNice)
    {
        if (dropEditIfTargetMoved(liveTarget) || !canApply(currentNice, liveTarget) || applying())
        {
            return;
        }
        // The edit's own target, which the checks above matched to @p liveTarget: the same PID and the
        // same, known start time.
        const Platform::ProcessTarget target = m_EditTarget;
        const std::int32_t nice = m_NiceValue;
        const std::int32_t revertTo = currentNice.value_or(Domain::Priority::NORMAL_NICE);
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        if (actions == nullptr)
        {
            finishApply(Platform::ProcessActionResult::error("Process actions unavailable"), revertTo);
            return;
        }
        m_Retry.reset();
        Platform::AuthorizedAction retry;
        retry.kind = Platform::AuthorizedAction::Kind::SetPriority;
        retry.target = target;
        retry.nice = nice;
        static_cast<void>(m_ApplyRun.start(
            std::format("Setting the priority of PID {}...", target.pid),
            [actions, target, nice] { return actions->setPriority(target, nice); },
            [this, generation = m_Generation, revertTo, actions, retry](const Platform::ProcessActionResult& result)
            {
                if (generation == m_Generation)
                {
                    finishApply(result, revertTo);
                    offerRetry(result, actions, retry, revertTo);
                }
            }));
    }

    /// Whether the error line offers "Authenticate and retry" (#2308): the last apply was refused for want
    /// of permission, the platform can ask for authorization, and nothing has changed since.
    [[nodiscard]] bool offersAuthenticateAndRetry() const noexcept
    {
        return m_Retry.has_value() && !applying();
    }

    /// "Authenticate and retry" was pressed: the refused change again, as an administrator, after the
    /// password prompt (IProcessActions::runAuthorized()), on the apply worker.
    void retryAuthorized()
    {
        if (!m_Retry.has_value() || !offersAuthenticateAndRetry())
        {
            return;
        }
        const Retry retry = *m_Retry;
        m_Retry.reset();
        static_cast<void>(m_ApplyRun.start(
            std::format("Waiting for authorization to set the priority of PID {}...", retry.action.target.pid),
            [actions = retry.actions, action = retry.action](const std::stop_token& stopToken)
            { return actions->runAuthorized(action, stopToken); },
            [this, generation = m_Generation, revertTo = retry.revertTo](const Platform::ProcessActionResult& result)
            {
                if (generation == m_Generation)
                {
                    finishApply(result, revertTo); // a refusal to the administrator isn't offered again
                }
            }));
    }

    /// Each frame, before drawing: a finished apply's result, its own and the I/O priority control's,
    /// goes to the error line. @p wait blocks until a running apply finishes (the tests' way to take it
    /// in deterministically).
    void update(bool wait = false)
    {
        m_ApplyRun.update(wait);
        m_IoPriorityView.update(wait);
    }

    /// Whether an apply runs on the worker now: Apply is disabled and the line under it says so.
    [[nodiscard]] bool applying() const noexcept
    {
        return m_ApplyRun.busy();
    }

    /// What the line under Apply says while applying(): "Setting the priority of PID 1234...".
    [[nodiscard]] const std::string& applyingText() const noexcept
    {
        return m_ApplyRun.pendingText();
    }

    /// The value the control shows: the edit, or the process's own nice value.
    [[nodiscard]] std::int32_t niceValue() const noexcept
    {
        return m_NiceValue;
    }

    /// Whether the shown value is an edit not yet applied.
    [[nodiscard]] bool hasPendingEdit() const noexcept
    {
        return m_Changed;
    }

    /// The process the pending edit was made for (PID -1 when nothing is pending).
    [[nodiscard]] const Platform::ProcessTarget& editTarget() const noexcept
    {
        return m_EditTarget;
    }

    /// The last failed apply's message, empty when there is none to show.
    [[nodiscard]] const std::string& error() const noexcept
    {
        return m_Error;
    }

    /// The I/O priority control drawn under the nice control where the platform can set it (#803).
    [[nodiscard]] ProcessIoPriorityView& ioPriorityView() noexcept
    {
        return m_IoPriorityView;
    }
    [[nodiscard]] const ProcessIoPriorityView& ioPriorityView() const noexcept
    {
        return m_IoPriorityView;
    }

  private:
    static constexpr Platform::ProcessTarget NO_TARGET{.pid = -1, .startTimeTicks = 0};

    /// Draws the priority slider (a stop per class on Windows); returns where it ends, for right-aligning Apply.
    /// @p labelled draws the "Priority" row label, with its tooltip, above it (labelsRows()).
    float renderSlider(std::optional<std::int32_t> currentNice, const Platform::ProcessTarget& target, bool labelled);
    /// Draws the priority slider, its Apply button and its error line; @p labelled as for renderSlider().
    void renderNiceControl(Platform::IProcessActions* actions,
                           std::optional<std::int32_t> currentNice,
                           const Platform::ProcessTarget& target,
                           bool labelled);
    void renderApplyButton(Platform::IProcessActions* actions,
                           std::optional<std::int32_t> currentNice,
                           const Platform::ProcessTarget& target,
                           float controlRightEdge);

    /// A permission refusal the platform can ask to redo (#2308): what "Authenticate and retry" sends,
    /// through the panel's actions (which outlive this view), and where a failure puts the control.
    struct Retry
    {
        Platform::AuthorizedAction action;
        Platform::IProcessActions* actions = nullptr;
        std::int32_t revertTo = Domain::Priority::NORMAL_NICE;
    };

    /// After an apply's result: offer "Authenticate and retry" when it was refused for want of
    /// permission and @p actions can ask for authorization.
    void offerRetry(const Platform::ProcessActionResult& result,
                    Platform::IProcessActions* actions,
                    const Platform::AuthorizedAction& action,
                    std::int32_t revertTo)
    {
        if (result.permissionDenied && actions->actionCapabilities().canAuthorizeInteractively)
        {
            m_Retry = Retry{.action = action, .actions = actions, .revertTo = revertTo};
        }
    }

    /// An apply's result, on the UI thread.
    void finishApply(const Platform::ProcessActionResult& result, std::int32_t revertTo)
    {
        if (result.success)
        {
            m_Error.clear();
            return;
        }
        m_Error = result.errorMessage; // Stays until the next edit, apply or selection change
        if (!m_Changed)
        {
            m_NiceValue = revertTo; // Back to the process's actual priority, since the change failed
        }
    }

    std::int32_t m_NiceValue = Domain::Priority::NORMAL_NICE;
    bool m_Changed = false;
    Platform::ProcessTarget m_EditTarget = NO_TARGET;
    std::string m_Error;            // Persistent error message for priority changes
    std::uint64_t m_Generation = 0; // Bumped by a selection change: a running apply's result is then dropped
    ProcessIoPriorityView m_IoPriorityView;
    std::optional<Retry> m_Retry;
    // Last, so it is destroyed (waiting for a running apply) before the state its finish writes.
    ProcessActionRun m_ApplyRun;
};

} // namespace App
