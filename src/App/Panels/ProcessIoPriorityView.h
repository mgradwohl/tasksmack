#pragma once

// Process Details' I/O priority control (#803): the Linux ionice setting, drawn by ProcessPriorityView
// under the nice control: one discrete slider through every class and level in bands -- Realtime 0..7 |
// Best-effort 0..7 | Idle (#1540) -- with the never-set default drawn hollow at the level nice derives, a
// "Reset to default" button, and its own Apply button.
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
#include "ProcessDetailsPanel_PriorityHelpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace App
{

namespace Detail
{

/// How often the shown process's I/O priority is read again while nothing else asks for a read, so
/// a change made outside TaskSmack (ionice) shows up.
inline constexpr double IO_PRIORITY_REFRESH_SECONDS = 2.0;

/// The classes the control can set: the default first, the privileged one last.
inline constexpr std::array<Platform::IoPriorityClass, 4> SETTABLE_IO_PRIORITY_CLASSES = {
    Platform::IoPriorityClass::None,
    Platform::IoPriorityClass::BestEffort,
    Platform::IoPriorityClass::Idle,
    Platform::IoPriorityClass::Realtime,
};

/// The class's name.
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

// ---------------------------------------------------------------------------------------------------
// The I/O priority slider (#1540): one discrete slider, highest priority first, in class bands --
// Realtime 0..7 | Best-effort 0..7 | Idle -- each stop named in ionice(1)'s words. Realtime needs
// CAP_SYS_NICE, so without it the slider starts at Best-effort 0 and a process already in Realtime is
// shown beyond the start, hollow, never picked (as Windows' Realtime class is, #1538).
// ---------------------------------------------------------------------------------------------------

/// Levels per class with levels (0 highest to 7 lowest).
inline constexpr std::int32_t IO_LEVEL_COUNT = Domain::Priority::MAX_IO_LEVEL - Domain::Priority::MIN_IO_LEVEL + 1;

/// The slider's ImGui ID, after "###" in every stop's label so it holds as the value moves.
inline constexpr const char* IO_PRIORITY_SLIDER_ID = "###io_priority_slider";

/// Every stop with Realtime, highest priority first. A stop's colour runs with the nice scale's: Realtime
/// in the high-priority colours, Best-effort through normal, Idle the lowest.
inline constexpr std::array<PrioritySliderStop, (2 * IO_LEVEL_COUNT) + 1> IO_PRIORITY_STOPS = {{
    {.name = "realtime 0", .itemLabel = "I/O priority: realtime 0###io_priority_slider", .colorNice = -20},
    {.name = "realtime 1", .itemLabel = "I/O priority: realtime 1###io_priority_slider", .colorNice = -19},
    {.name = "realtime 2", .itemLabel = "I/O priority: realtime 2###io_priority_slider", .colorNice = -18},
    {.name = "realtime 3", .itemLabel = "I/O priority: realtime 3###io_priority_slider", .colorNice = -17},
    {.name = "realtime 4", .itemLabel = "I/O priority: realtime 4###io_priority_slider", .colorNice = -16},
    {.name = "realtime 5", .itemLabel = "I/O priority: realtime 5###io_priority_slider", .colorNice = -15},
    {.name = "realtime 6", .itemLabel = "I/O priority: realtime 6###io_priority_slider", .colorNice = -14},
    {.name = "realtime 7", .itemLabel = "I/O priority: realtime 7###io_priority_slider", .colorNice = -13},
    {.name = "best-effort 0", .itemLabel = "I/O priority: best-effort 0###io_priority_slider", .colorNice = -9},
    {.name = "best-effort 1", .itemLabel = "I/O priority: best-effort 1###io_priority_slider", .colorNice = -6},
    {.name = "best-effort 2", .itemLabel = "I/O priority: best-effort 2###io_priority_slider", .colorNice = -3},
    {.name = "best-effort 3", .itemLabel = "I/O priority: best-effort 3###io_priority_slider", .colorNice = 0},
    {.name = "best-effort 4", .itemLabel = "I/O priority: best-effort 4###io_priority_slider", .colorNice = 3},
    {.name = "best-effort 5", .itemLabel = "I/O priority: best-effort 5###io_priority_slider", .colorNice = 6},
    {.name = "best-effort 6", .itemLabel = "I/O priority: best-effort 6###io_priority_slider", .colorNice = 9},
    {.name = "best-effort 7", .itemLabel = "I/O priority: best-effort 7###io_priority_slider", .colorNice = 12},
    {.name = "idle", .itemLabel = "I/O priority: idle###io_priority_slider", .colorNice = 19},
}};

/// The Realtime state a process can be in without TaskSmack being able to set it: shown beyond the
/// start of the slider without Realtime, hollow.
inline constexpr PrioritySliderStop IO_REALTIME_BEYOND_STOP{
    .name = "realtime", .itemLabel = "I/O priority: realtime###io_priority_slider", .colorNice = -20};

/// Where the Best-effort and Idle bands begin, with Realtime's stops and without them.
inline constexpr std::array<std::int32_t, 2> IO_BANDS_WITH_REALTIME = {IO_LEVEL_COUNT, 2 * IO_LEVEL_COUNT};
inline constexpr std::array<std::int32_t, 1> IO_BANDS_WITHOUT_REALTIME = {IO_LEVEL_COUNT};

inline constexpr const char* IO_PRIORITY_SLIDER_TOOLTIP = "I/O scheduling class and level (ionice):\n"
                                                          "  Realtime: served before everything else; needs CAP_SYS_NICE (or root)\n"
                                                          "  Best-effort: shares the disk by level, 0 (highest) to 7 (lowest)\n"
                                                          "  Idle: gets the disk only when no other process wants it\n"
                                                          "A hollow thumb is the default, derived from the nice value.\n\n"
                                                          "Keyboard shortcuts:\n"
                                                          "  Left/Right, Up/Down: One step higher or lower\n"
                                                          "  Home/End: Highest/lowest";

/// The slider with every class: CAP_SYS_NICE (or root) may set Realtime.
inline constexpr DiscretePrioritySlider IO_PRIORITY_SLIDER{
    .stops = IO_PRIORITY_STOPS,
    .beyondStart = nullptr,
    .tooltip = IO_PRIORITY_SLIDER_TOOLTIP,
    .bandStarts = IO_BANDS_WITH_REALTIME,
};

/// The slider without Realtime, which needs a privilege TaskSmack lacks: Best-effort 0 first.
inline constexpr DiscretePrioritySlider IO_PRIORITY_SLIDER_NO_REALTIME{
    .stops = std::span<const PrioritySliderStop>(IO_PRIORITY_STOPS).subspan(IO_LEVEL_COUNT),
    .beyondStart = &IO_REALTIME_BEYOND_STOP,
    .tooltip = IO_PRIORITY_SLIDER_TOOLTIP,
    .bandStarts = IO_BANDS_WITHOUT_REALTIME,
};

/// Where the slider shows @p priority, and whether that is the inherited default (drawn hollow).
struct IoStop
{
    std::int32_t index = 0; ///< A stop, or PRIORITY_STOP_BEYOND_START for Realtime the slider lacks
    bool inherited = false; ///< Never set (IoPriorityClass::None): at the Best-effort level nice derives
};

/// The stop for @p priority on the slider with Realtime (@p realtimeSettable) or without. A class never
/// set sits at the Best-effort level the kernel derives from @p nice (ioLevelForNice()), marked inherited.
[[nodiscard]] constexpr IoStop ioStopFor(const Platform::IoPriority& priority, std::int32_t nice, bool realtimeSettable) noexcept
{
    const std::int32_t bestEffortStart = realtimeSettable ? IO_LEVEL_COUNT : 0;
    const std::int32_t level = Domain::Priority::clampIoLevel(priority.level);
    switch (priority.ioClass)
    {
    case Platform::IoPriorityClass::Realtime:
        return {.index = realtimeSettable ? level : PRIORITY_STOP_BEYOND_START, .inherited = false};
    case Platform::IoPriorityClass::BestEffort:
        return {.index = bestEffortStart + level, .inherited = false};
    case Platform::IoPriorityClass::Idle:
        return {.index = bestEffortStart + IO_LEVEL_COUNT, .inherited = false};
    case Platform::IoPriorityClass::None:
    default:
        return {.index = bestEffortStart + Domain::Priority::ioLevelForNice(nice), .inherited = true};
    }
}

/// The class and level stop @p index stands for on the slider with Realtime (@p realtimeSettable) or
/// without: ioStopFor()'s inverse for every stop (an explicit class, never None).
[[nodiscard]] constexpr Platform::IoPriority ioPriorityForStop(std::int32_t index, bool realtimeSettable) noexcept
{
    const std::int32_t stopCount = realtimeSettable ? (2 * IO_LEVEL_COUNT) + 1 : IO_LEVEL_COUNT + 1;
    std::int32_t i = std::clamp(index, 0, stopCount - 1);
    if (realtimeSettable)
    {
        if (i < IO_LEVEL_COUNT)
        {
            return {.ioClass = Platform::IoPriorityClass::Realtime, .level = i};
        }
        i -= IO_LEVEL_COUNT;
    }
    if (i < IO_LEVEL_COUNT)
    {
        return {.ioClass = Platform::IoPriorityClass::BestEffort, .level = i};
    }
    return {.ioClass = Platform::IoPriorityClass::Idle, .level = 0};
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
    /// then the row label, one slider through every class and level (Detail::IO_PRIORITY_SLIDER, or
    /// without Realtime unless @p realtimeSettable, #1540), "Reset to default" and Apply, which sets the
    /// edit through @p actions. @p currentNice is the process's nice value from its latest snapshot: a
    /// class never set shows at the Best-effort level it derives. The caller checks the capability.
    void render(Platform::IProcessActions* actions,
                std::optional<std::int32_t> currentNice,
                const Platform::ProcessTarget& target,
                bool realtimeSettable);

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
