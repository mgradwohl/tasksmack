#pragma once

// Process Details' I/O priority control (#803): the Linux ionice setting, drawn by ProcessPriorityView
// under the nice control. A class combo (Default / Best-effort / Idle / Realtime), a level slider for the
// classes that have levels, its own Apply button, and the process's current class and level.
//
// The safety model is ProcessPriorityView's, rule for rule: an edit records the process it was made for
// (PID and start time); Apply is enabled only when the edit's and the live target's start times are
// both known and equal (Detail::isSameEditTarget()); a selection change, or the live target becoming a
// different process, drops the edit; and Apply ends the edit, so it is never replayed.
//
// The current I/O priority is not a sampled counter: it is read on demand through
// IProcessActions::getIoPriority(), for the shown process only -- when the target changes, after an
// apply, and otherwise at most every IO_PRIORITY_REFRESH_SECONDS while the control is drawn.
//
// Everything but render() is defined here, free of ImGui, so the state can be tested against a mock
// IProcessActions without an ImGui context (test_ProcessIoPriorityView.cpp).

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "PriorityEditTarget.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace App
{

namespace Detail
{

/// How often the shown process's I/O priority is read again while nothing else asks for a read, so
/// a change made outside TaskSmack (ionice) shows up.
inline constexpr double IO_PRIORITY_REFRESH_SECONDS = 2.0;

/// Width of the I/O class combo, in ems: room for its longest name, "Best-effort", plus the arrow.
inline constexpr float IO_PRIORITY_CLASS_COMBO_WIDTH_EM = 9.0F;

/// Width of the level slider, in ems.
inline constexpr float IO_PRIORITY_LEVEL_SLIDER_WIDTH_EM = 9.0F;

/// How far the combo and the slider may shrink before the row gives up a line instead: two thirds of
/// their authored widths (6 em each at the default sizes).
inline constexpr float IO_PRIORITY_MIN_WIDTH_FRACTION = 2.0F / 3.0F;

/// Where the I/O priority row's items go and how wide they are, for a panel @p available pixels wide.
struct IoPriorityRowLayout
{
    float comboWidth = 0.0F;      ///< The class combo.
    float sliderWidth = 0.0F;     ///< The level slider (0 when the class has none).
    float applyWidth = 0.0F;      ///< The Apply button.
    bool sliderOnNewLine = false; ///< The slider sits under the combo rather than beside it.
    bool applyOnNewLine = false;  ///< Apply sits under the controls rather than after them.
};

/// The combo and the slider (when @p hasSlider) side by side, at their authored widths times @p scale.
[[nodiscard]] constexpr float ioControlsWidth(float emPx, float spacing, bool hasSlider, float scale) noexcept
{
    const float combo = IO_PRIORITY_CLASS_COMBO_WIDTH_EM * emPx * scale;
    return hasSlider ? combo + spacing + (IO_PRIORITY_LEVEL_SLIDER_WIDTH_EM * emPx * scale) : combo;
}

/// Lays out the I/O priority row so nothing is clipped: Process Details does not scroll horizontally.
/// As the nice control does, Apply's width (@p applyNaturalWidth, capped to the panel) is reserved
/// first; the combo and slider get what is left, at their authored widths when they fit, shrunk
/// proportionally down to IO_PRIORITY_MIN_WIDTH_FRACTION when not. Narrower than that, Apply moves to
/// its own line and the controls get the whole width, shrinking again; narrower still, the slider
/// moves under the combo and each takes at most the panel's width. @p spacing is ImGui's item spacing.
[[nodiscard]] constexpr IoPriorityRowLayout
computeIoPriorityRowLayout(float available, float emPx, float spacing, float applyNaturalWidth, bool hasSlider) noexcept
{
    const float width = std::max(available, 1.0F);
    IoPriorityRowLayout layout;
    layout.applyWidth = std::min(applyNaturalWidth, width);

    const float idealControls = ioControlsWidth(emPx, spacing, hasSlider, 1.0F);
    const float minControls = ioControlsWidth(emPx, spacing, hasSlider, IO_PRIORITY_MIN_WIDTH_FRACTION);
    const float gaps = hasSlider ? spacing : 0.0F;

    // The room for the controls: beside Apply when they fit there at their minimum, else a line of their own.
    float room = width - spacing - layout.applyWidth;
    if (room < minControls)
    {
        layout.applyOnNewLine = true;
        room = width;
    }

    float scale = 1.0F;
    if (room < idealControls)
    {
        scale = (room - gaps) / (idealControls - gaps);
    }
    if (room < minControls)
    {
        // Too narrow for the two side by side even alone: stack them, each up to the panel's width.
        layout.sliderOnNewLine = hasSlider;
        layout.comboWidth = std::min(IO_PRIORITY_CLASS_COMBO_WIDTH_EM * emPx, width);
        layout.sliderWidth = hasSlider ? std::min(IO_PRIORITY_LEVEL_SLIDER_WIDTH_EM * emPx, width) : 0.0F;
        return layout;
    }
    layout.comboWidth = IO_PRIORITY_CLASS_COMBO_WIDTH_EM * emPx * scale;
    layout.sliderWidth = hasSlider ? IO_PRIORITY_LEVEL_SLIDER_WIDTH_EM * emPx * scale : 0.0F;
    return layout;
}

/// The classes the combo offers, in its order: the default first, the privileged one last.
inline constexpr std::array<Platform::IoPriorityClass, 4> SETTABLE_IO_PRIORITY_CLASSES = {
    Platform::IoPriorityClass::None,
    Platform::IoPriorityClass::BestEffort,
    Platform::IoPriorityClass::Idle,
    Platform::IoPriorityClass::Realtime,
};

/// The class's name in the combo.
[[nodiscard]] constexpr std::string_view ioPriorityClassName(Platform::IoPriorityClass ioClass) noexcept
{
    switch (ioClass)
    {
    case Platform::IoPriorityClass::Realtime:
        return "Realtime";
    case Platform::IoPriorityClass::BestEffort:
        return "Best-effort";
    case Platform::IoPriorityClass::Idle:
        return "Idle";
    case Platform::IoPriorityClass::None:
    default:
        return "Default";
    }
}

/// Whether @p ioClass has levels (Realtime and Best-effort), so the level slider is shown for it.
[[nodiscard]] constexpr bool ioClassHasLevels(Platform::IoPriorityClass ioClass) noexcept
{
    return ioClass == Platform::IoPriorityClass::Realtime || ioClass == Platform::IoPriorityClass::BestEffort;
}

/// @p priority as the platform keeps it: the level held to 0-7 for a class with levels, 0 otherwise.
[[nodiscard]] constexpr Platform::IoPriority normalizeIoPriority(const Platform::IoPriority& priority) noexcept
{
    return {
        .ioClass = priority.ioClass,
        .level = ioClassHasLevels(priority.ioClass) ? Domain::Priority::clampIoLevel(priority.level) : 0,
    };
}

/// The current I/O priority as the header shows it, in ionice(1)'s words: "best-effort 4", "idle",
/// "realtime 0", or for a class never set "default (best-effort N from nice)", N being the level the
/// kernel derives from @p nice (just "default (from nice)" while the nice value is not known).
[[nodiscard]] inline std::string describeIoPriority(const Platform::IoPriority& priority, std::optional<std::int32_t> nice)
{
    switch (priority.ioClass)
    {
    case Platform::IoPriorityClass::Realtime:
        return std::format("realtime {}", priority.level);
    case Platform::IoPriorityClass::BestEffort:
        return std::format("best-effort {}", priority.level);
    case Platform::IoPriorityClass::Idle:
        return "idle";
    case Platform::IoPriorityClass::None:
    default:
        return nice.has_value() ? std::format("default (best-effort {} from nice)", Domain::Priority::ioLevelForNice(*nice))
                                : std::string("default (from nice)");
    }
}

} // namespace Detail

/// The I/O priority control for the process Process Details shows (Linux; hidden where
/// ProcessActionCapabilities::canSetIoPriority is false).
class ProcessIoPriorityView
{
  public:
    /// Draws the control for @p target: reads its current I/O priority when due (refreshCurrent()),
    /// then the header, the class combo, the level slider (Best-effort and Realtime) and Apply, which
    /// sets the edit through @p actions. @p currentNice is the process's nice value from its latest
    /// snapshot, used only to name the level a never-set class derives from it. The caller checks the
    /// capability.
    void render(Platform::IProcessActions* actions, std::optional<std::int32_t> currentNice, const Platform::ProcessTarget& target);

    /// A different process was selected: drop the edit, its target, the error line and the value read
    /// for the previous process.
    void onSelectionChanged() noexcept
    {
        m_Edit = {};
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        m_Error.clear();
        m_Current.reset();
        m_CurrentTarget = NO_TARGET;
        m_ReadError.clear();
        m_ReadDue = true;
    }

    /// Reads @p target's current I/O priority through @p actions when it is due: the target is not
    /// the one last read, a read was asked for (after an apply or a selection change), or
    /// IO_PRIORITY_REFRESH_SECONDS have passed since the last read at @p nowSeconds. A target whose
    /// start time is not known yet is not read -- every IProcessActions refuses it -- and has no
    /// current value. On a different target the shown value goes back to the neutral default before the
    /// read, so if the read fails the control never shows the previous process's class and level.
    /// Returns whether it read (or cleared) the value.
    bool refreshCurrent(Platform::IProcessActions* actions, const Platform::ProcessTarget& target, double nowSeconds)
    {
        const bool sameTarget = (m_CurrentTarget.pid == target.pid) && (m_CurrentTarget.startTimeTicks == target.startTimeTicks);
        if (sameTarget && !m_ReadDue && (nowSeconds - m_LastReadSeconds) < Detail::IO_PRIORITY_REFRESH_SECONDS)
        {
            return false;
        }
        if (!sameTarget && !m_Changed)
        {
            m_Edit = {};
        }
        m_CurrentTarget = target;
        m_LastReadSeconds = nowSeconds;
        m_ReadDue = false;
        m_Current.reset();
        m_ReadError.clear();
        if (actions == nullptr || target.pid <= 0 || target.startTimeTicks == 0)
        {
            return true;
        }
        const Platform::IoPriorityReadResult read = actions->getIoPriority(target);
        if (read.has_value())
        {
            m_Current = Detail::normalizeIoPriority(*read);
        }
        else
        {
            m_ReadError = read.error();
        }
        return true;
    }

    /// While nothing is edited, the control follows the process's own I/O priority, once read.
    void syncToProcess() noexcept
    {
        if (!m_Changed && m_Current.has_value())
        {
            m_Edit = *m_Current;
        }
    }

    /// The user picked @p priority (normalised: Detail::normalizeIoPriority()) for @p target. A value
    /// other than the shown one becomes a pending edit for @p target and clears the error line; the
    /// same value changes nothing.
    void editIoPriority(const Platform::IoPriority& priority, const Platform::ProcessTarget& target)
    {
        const Platform::IoPriority normalized = Detail::normalizeIoPriority(priority);
        if (normalized == m_Edit)
        {
            return;
        }
        m_Edit = normalized;
        m_Changed = true;
        m_EditTarget = target;
        m_Error.clear();
    }

    /// The user picked class @p ioClass in the combo for @p target. A class with levels keeps the
    /// shown level when the shown class has one too; otherwise it starts at the level the kernel
    /// derives from @p currentNice (4 at nice 0), which is what a never-set class already gets.
    void editClass(Platform::IoPriorityClass ioClass, std::optional<std::int32_t> currentNice, const Platform::ProcessTarget& target)
    {
        const std::int32_t level = Detail::ioClassHasLevels(m_Edit.ioClass)
                                     ? m_Edit.level
                                     : Domain::Priority::ioLevelForNice(currentNice.value_or(Domain::Priority::NORMAL_NICE));
        editIoPriority({.ioClass = ioClass, .level = level}, target);
    }

    /// Drops a pending edit that may be for a process other than @p liveTarget, as
    /// ProcessPriorityView::dropEditIfTargetMoved(), and the edited class and level with it: the control
    /// shows the neutral default until @p liveTarget's own value is read, never the edit made for another
    /// process. Returns whether it did.
    bool dropEditIfTargetMoved(const Platform::ProcessTarget& liveTarget) noexcept
    {
        if (!m_Changed || Detail::isSameEditTarget(m_EditTarget, liveTarget))
        {
            return false;
        }
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        m_Edit = {};
        return true;
    }

    /// Whether Apply is enabled for @p liveTarget: there is a pending edit, the current I/O priority
    /// of @p liveTarget has been read (the counterpart of the nice control's snapshot), both the
    /// edit's target and @p liveTarget know the start time, and they are the same process
    /// (Detail::isSameEditTarget()).
    [[nodiscard]] bool canApply(const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_Changed && hasCurrentFor(liveTarget) && m_EditTarget.startTimeTicks != 0 && liveTarget.startTimeTicks != 0 &&
               Detail::isSameEditTarget(m_EditTarget, liveTarget);
    }

    /// Whether there is a pending edit that Apply cannot send yet for want of process details.
    [[nodiscard]] bool waitingForProcessDetails(const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_Changed && !canApply(liveTarget);
    }

    /// Apply was pressed with @p liveTarget shown: set the edited class and level on the edit's own
    /// target through @p actions, as ProcessPriorityView::apply(). Success clears the error line;
    /// failure shows the platform's message and puts the control back at the current value. Either
    /// way the edit is finished and a fresh read is asked for, so the control shows what the process
    /// really has.
    void apply(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
    {
        if (dropEditIfTargetMoved(liveTarget) || !canApply(liveTarget))
        {
            return;
        }
        const Platform::ProcessTarget target = m_EditTarget;
        const Platform::IoPriority priority = m_Edit;
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        m_ReadDue = true;

        const Platform::ProcessActionResult result = (actions != nullptr)
                                                       ? actions->setIoPriority(target, priority.ioClass, priority.level)
                                                       : Platform::ProcessActionResult::error("Process actions unavailable");
        if (result.success)
        {
            m_Error.clear();
        }
        else
        {
            m_Error = result.errorMessage; // Stays until the next edit, apply or selection change
            m_Edit = m_Current.value_or(Platform::IoPriority{});
        }
    }

    /// The class and level the control shows: the edit, or the process's own.
    [[nodiscard]] const Platform::IoPriority& shownIoPriority() const noexcept
    {
        return m_Edit;
    }

    /// The process's I/O priority as last read, or nullopt before a read or when it failed.
    [[nodiscard]] const std::optional<Platform::IoPriority>& currentIoPriority() const noexcept
    {
        return m_Current;
    }

    /// Why the last read failed, empty when it did not.
    [[nodiscard]] const std::string& readError() const noexcept
    {
        return m_ReadError;
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

  private:
    static constexpr Platform::ProcessTarget NO_TARGET{.pid = -1, .startTimeTicks = 0};

    /// Whether m_Current was read for exactly @p liveTarget.
    [[nodiscard]] bool hasCurrentFor(const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_Current.has_value() && m_CurrentTarget.pid == liveTarget.pid &&
               m_CurrentTarget.startTimeTicks == liveTarget.startTimeTicks;
    }

    /// Draws the class combo and, for a class with levels, the level slider, sized and placed by @p layout.
    void renderControls(std::optional<std::int32_t> currentNice,
                        const Platform::ProcessTarget& target,
                        const Detail::IoPriorityRowLayout& layout);

    Platform::IoPriority m_Edit;
    bool m_Changed = false;
    Platform::ProcessTarget m_EditTarget = NO_TARGET;
    std::string m_Error;

    std::optional<Platform::IoPriority> m_Current;
    Platform::ProcessTarget m_CurrentTarget = NO_TARGET;
    std::string m_ReadError;
    double m_LastReadSeconds = 0.0;
    bool m_ReadDue = true;
};

} // namespace App
