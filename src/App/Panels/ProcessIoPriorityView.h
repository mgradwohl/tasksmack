#pragma once

// Process Details' I/O priority control (#803): the Linux ionice setting, drawn by ProcessPriorityView
// under the nice control: one discrete slider through every class and level in bands -- Realtime 0..7 |
// Best-effort 0..7 | Idle (#1540) -- with the never-set default drawn hollow at the level nice derives, a
// "Reset to default" button, and its own Apply button. On Windows (#3362) the same control sets the I/O
// priority hint instead: High | Normal | Low | Very low, with no Reset to default.
//
// The safety model is ProcessPriorityView's, rule for rule: an edit records the process it was made for
// (PID and start time); Apply is enabled only when the edit's and the live target's start times are
// both known and equal (Detail::isSameEditTarget()); a selection change, or the live target becoming a
// different process, drops the edit; and Apply ends the edit, so it is never replayed.
//
// The current I/O priority is not a sampled counter: it is read on demand through
// IProcessActions::getIoPriority(), for the shown process only -- when the target changes, after an
// apply, and otherwise every Domain::Sampling::PROCESS_IO_PRIORITY_REFRESH_MS while the control is
// drawn. The read runs on a worker (Detail::LazyBackgroundRead), started from the panel's update path
// by updateRead(), never from render(): on Linux it opens a pidfd, reads /proc and calls ioprio_get
// (#2734). render() only draws the last value taken in.
//
// Apply runs on a worker of its own (ProcessActionRun, #2305), as ProcessPriorityView's does; update()
// takes its result in on a later frame, and only then is the current value read again.
//
// Everything but render() is defined here, free of ImGui, so the state can be tested against a mock
// IProcessActions without an ImGui context (test_ProcessIoPriorityView.cpp).

#include "App/Panels/LazyBackgroundRead.h"
#include "App/Panels/ProcessActionRun.h"
#include "Domain/PriorityConfig.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"
#include "PriorityEditTarget.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace Detail
{

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
    case Platform::IoPriorityClass::WindowsHint:
        return "Priority hint";
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

/// @p priority as the platform keeps it: the level held to 0-7 for a class with levels, a Windows hint held to
/// Very low to Critical (#3362), 0 otherwise.
[[nodiscard]] constexpr Platform::IoPriority normalizeIoPriority(const Platform::IoPriority& priority) noexcept
{
    if (priority.ioClass == Platform::IoPriorityClass::WindowsHint)
    {
        return {
            .ioClass = priority.ioClass,
            .level = std::clamp(priority.level,
                                static_cast<std::int32_t>(Platform::IoPriorityHint::VeryLow),
                                static_cast<std::int32_t>(Platform::IoPriorityHint::Critical)),
        };
    }
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
inline constexpr std::array<PrioritySliderStop, (2 * IO_LEVEL_COUNT) + 1> IO_PRIORITY_STOPS = {
    {
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
    },
};

/// The Realtime state a process can be in without TaskSmack being able to set it: shown beyond the
/// start of the slider without Realtime, hollow.
inline constexpr PrioritySliderStop IO_REALTIME_BEYOND_STOP{
    .name = "realtime",
    .itemLabel = "I/O priority: realtime###io_priority_slider",
    .colorNice = -20,
};

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

// ---------------------------------------------------------------------------------------------------
// Windows' I/O priority (#3362): one hint, not classes and levels, so its own slider -- High | Normal |
// Low | Very low, highest first as the nice slider -- in the words Process Explorer and System Informer
// use. Critical, the memory manager's own, is never offered: a process already there (they are the
// system's) is shown beyond the start, hollow, as Windows' Realtime priority class is (#1538).
// ---------------------------------------------------------------------------------------------------

/// The hint's name as the control shows it: "Very low", "Low", "Normal", "High", "Critical".
[[nodiscard]] constexpr std::string_view ioPriorityHintName(Platform::IoPriorityHint hint) noexcept
{
    switch (hint)
    {
    case Platform::IoPriorityHint::VeryLow:
        return "Very low";
    case Platform::IoPriorityHint::Low:
        return "Low";
    case Platform::IoPriorityHint::High:
        return "High";
    case Platform::IoPriorityHint::Critical:
        return "Critical";
    case Platform::IoPriorityHint::Normal:
        break;
    }
    return "Normal";
}

/// The hints the slider offers, highest first (stop 0 is High).
inline constexpr std::array<Platform::IoPriorityHint, 4> IO_HINT_ORDER = {
    Platform::IoPriorityHint::High,
    Platform::IoPriorityHint::Normal,
    Platform::IoPriorityHint::Low,
    Platform::IoPriorityHint::VeryLow,
};

/// Its stops, coloured as the nice scale: High in the high-priority colours, Very low the lowest.
inline constexpr std::array<PrioritySliderStop, IO_HINT_ORDER.size()> IO_HINT_STOPS = {
    {
        {.name = "High", .itemLabel = "I/O priority: High###io_priority_slider", .colorNice = -10},
        {.name = "Normal", .itemLabel = "I/O priority: Normal###io_priority_slider", .colorNice = 0},
        {.name = "Low", .itemLabel = "I/O priority: Low###io_priority_slider", .colorNice = 10},
        {.name = "Very low", .itemLabel = "I/O priority: Very low###io_priority_slider", .colorNice = 19},
    },
};

/// Critical, shown beyond the start, hollow, never picked.
inline constexpr PrioritySliderStop IO_HINT_CRITICAL_BEYOND_STOP{
    .name = "Critical",
    .itemLabel = "I/O priority: Critical###io_priority_slider",
    .colorNice = -20,
};

inline constexpr const char* IO_HINT_SLIDER_TOOLTIP = "Windows I/O priority:\n"
                                                      "  High: served before Normal; needs administrator rights\n"
                                                      "  Normal: every process's default\n"
                                                      "  Low, Very low: background work, served after Normal\n"
                                                      "Critical is the system's own, and isn't offered.\n\n"
                                                      "Keyboard shortcuts:\n"
                                                      "  Left/Right, Up/Down: One step higher or lower\n"
                                                      "  Home/End: Highest/lowest";

/// The Windows slider (#3362).
inline constexpr DiscretePrioritySlider IO_HINT_SLIDER{
    .stops = IO_HINT_STOPS,
    .beyondStart = &IO_HINT_CRITICAL_BEYOND_STOP,
    .tooltip = IO_HINT_SLIDER_TOOLTIP,
    .bandStarts = {},
};

/// The stop for @p priority on the Windows slider: Critical beyond its start (PRIORITY_STOP_BEYOND_START), and
/// anything not yet read as a Windows hint at Normal, every process's default.
[[nodiscard]] constexpr std::int32_t ioHintStopFor(const Platform::IoPriority& priority) noexcept
{
    if (priority.ioClass != Platform::IoPriorityClass::WindowsHint)
    {
        return 1; // Normal
    }
    const std::optional<Platform::IoPriorityHint> hint = Platform::ioPriorityHintFromLevel(priority.level);
    if (!hint.has_value())
    {
        return 1; // Normal: a level Windows doesn't define
    }
    // IO_HINT_ORDER's stops, spelled out so nothing here can throw.
    switch (*hint)
    {
    case Platform::IoPriorityHint::Critical:
        return PRIORITY_STOP_BEYOND_START;
    case Platform::IoPriorityHint::High:
        return 0;
    case Platform::IoPriorityHint::Low:
        return 2;
    case Platform::IoPriorityHint::VeryLow:
        return 3;
    case Platform::IoPriorityHint::Normal:
        break;
    }
    return 1;
}

/// The hint stop @p index stands for: ioHintStopFor()'s inverse (never Critical); held to the stops.
[[nodiscard]] constexpr Platform::IoPriority ioHintForStop(std::int32_t index) noexcept
{
    Platform::IoPriorityHint hint = Platform::IoPriorityHint::Normal;
    if (index <= 0)
    {
        hint = Platform::IoPriorityHint::High;
    }
    else if (index == 2)
    {
        hint = Platform::IoPriorityHint::Low;
    }
    else if (index >= 3)
    {
        hint = Platform::IoPriorityHint::VeryLow;
    }
    return {.ioClass = Platform::IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(hint)};
}

// The two functions above and the slider's stops agree, stop by stop.
static_assert(IO_HINT_ORDER[0] == Platform::IoPriorityHint::High && IO_HINT_ORDER[1] == Platform::IoPriorityHint::Normal &&
              IO_HINT_ORDER[2] == Platform::IoPriorityHint::Low && IO_HINT_ORDER[3] == Platform::IoPriorityHint::VeryLow);

/// The current I/O priority as the header shows it, in ionice(1)'s words: "best-effort 4", "idle",
/// "realtime 0", or for a class never set "default (best-effort N from nice)", N being the level the
/// kernel derives from @p nice (just "default (from nice)" while the nice value is not known). A Windows
/// hint (#3362) is its name: "Very low", "Normal", "High".
[[nodiscard]] inline std::string describeIoPriority(const Platform::IoPriority& priority, std::optional<std::int32_t> nice)
{
    switch (priority.ioClass)
    {
    case Platform::IoPriorityClass::WindowsHint:
    {
        const std::optional<Platform::IoPriorityHint> hint = Platform::ioPriorityHintFromLevel(priority.level);
        return hint.has_value() ? std::string(ioPriorityHintName(*hint)) : std::format("unknown ({})", priority.level);
    }
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

/// The I/O priority control for the process Process Details shows (Linux, and Windows since #3362; hidden where
/// ProcessActionCapabilities::canSetIoPriority is false).
class ProcessIoPriorityView
{
  public:
    /// Draws the control for @p target from the last value updateRead() took in: the row label, one slider through every class and level
    /// (Detail::IO_PRIORITY_SLIDER, or without Realtime unless @p realtimeSettable, #1540), "Reset to default" and Apply, which sets the
    /// edit through @p actions. @p currentNice is the process's nice value from its latest snapshot: a
    /// class never set shows at the Best-effort level it derives. With @p windowsHints
    /// (ProcessActionCapabilities::ioPriorityHints, #3362) it is Windows' slider instead
    /// (Detail::IO_HINT_SLIDER), with no Reset to default: Normal is every process's default. The caller checks
    /// the capability.
    void render(Platform::IProcessActions* actions,
                std::optional<std::int32_t> currentNice,
                const Platform::ProcessTarget& target,
                bool realtimeSettable,
                bool windowsHints = false);

    /// A different process was selected: drop the edit, its target, the error line and the value read
    /// for the previous process.
    void onSelectionChanged() noexcept
    {
        ++m_Generation; // an apply still running reports to nobody: its process is no longer shown
        m_Retry.reset();
        m_Edit = {};
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        m_Error.clear();
        m_Current.reset();
        m_CurrentTarget = NO_TARGET;
        m_ReadError.clear();
        m_Read.reset(); // a read in flight was for the previous process
    }

    /// Each frame, from the panel's update path (never render()): takes in a read that finished for
    /// @p target, then starts the next read of @p target's current I/O priority through @p actions on
    /// the worker when it is due -- the control was drawn last frame, and no read has been started since
    /// it showed, the target changed or an apply finished, or PROCESS_IO_PRIORITY_REFRESH_MS have passed
    /// (@p deltaSeconds is the frame's time step). On a different target the shown value goes back to
    /// the neutral default at once, so the control never shows the previous process's class and level.
    /// A target whose start time is not known yet is not read -- every IProcessActions refuses it -- and
    /// has no current value. @p wait blocks until a read in flight finishes (the tests' way to take it
    /// in deterministically). @p actions must outlive the read; the panel owns them. Returns whether a
    /// read started.
    bool updateRead(Platform::IProcessActions* actions, const Platform::ProcessTarget& target, float deltaSeconds, bool wait = false)
    {
        if (std::optional<Platform::IoPriorityReadResult> read = m_Read.takeFinished(target, wait))
        {
            m_Current.reset();
            m_ReadError.clear();
            if (read->has_value())
            {
                m_Current = Detail::normalizeIoPriority(**read);
            }
            else
            {
                m_ReadError = read->error();
            }
        }
        const bool sameTarget = (m_CurrentTarget.pid == target.pid) && (m_CurrentTarget.startTimeTicks == target.startTimeTicks);
        if (!sameTarget)
        {
            if (!m_Changed)
            {
                m_Edit = {};
            }
            m_CurrentTarget = target;
            m_Current.reset();
            m_ReadError.clear();
            m_Read.rearm(); // read the new target now; a read of the old one is dropped when it lands
        }
        if (!m_Read.due(deltaSeconds) || actions == nullptr || target.pid <= 0 || target.startTimeTicks == 0)
        {
            return false;
        }
        if (std::optional<Platform::IoPriorityReadResult> failed =
                m_Read.start(target, [actions](const Platform::ProcessTarget& t) { return actions->getIoPriority(t); }))
        {
            m_ReadError = failed->error();
        }
        return true;
    }

    /// render() drew the control this frame, so the next updateRead() may start a read.
    void markDrawn() noexcept
    {
        m_Read.markDrawnOpen();
    }

    /// Whether a read runs on the worker now.
    [[nodiscard]] bool reading() const noexcept
    {
        return m_Read.inFlight();
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
        m_Retry.reset(); // a new edit replaces the refused one
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
    /// of @p liveTarget has been read (the counterpart of the nice control's snapshot), or its read was
    /// refused for want of access while allowApplyWhenReadDenied() is on, both the
    /// edit's target and @p liveTarget know the start time, and they are the same process
    /// (Detail::isSameEditTarget()).
    [[nodiscard]] bool canApply(const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_Changed && (hasCurrentFor(liveTarget) || readDeniedFor(liveTarget)) && m_EditTarget.startTimeTicks != 0 &&
               liveTarget.startTimeTicks != 0 && Detail::isSameEditTarget(m_EditTarget, liveTarget);
    }

    /// Lets Apply go ahead with the current value unknown when the read was refused for want of access
    /// (Platform::isIoPriorityReadDenied(), #3362): on Windows another user's or an elevated process often
    /// refuses the read, while the set, identity-checked as every action, can still be made through the
    /// elevated helper. render() turns it on with Windows' hints where the actions can retry as an
    /// administrator (ProcessActionCapabilities::canAuthorizeInteractively).
    void allowApplyWhenReadDenied(bool allow) noexcept
    {
        m_ApplyWhenReadDenied = allow;
    }

    /// Whether there is a pending edit that Apply cannot send yet for want of process details.
    [[nodiscard]] bool waitingForProcessDetails(const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_Changed && !canApply(liveTarget);
    }

    /// Apply was pressed with @p liveTarget shown: set the edited class and level on the edit's own
    /// target through @p actions, as ProcessPriorityView::apply(), on the apply worker (#2305). Success
    /// clears the error line; failure shows the platform's message and, unless the value has been edited
    /// again meanwhile, puts the control back at the current value. Either way the edit is finished and,
    /// once the result is in, a fresh read is asked for, so the control shows what the process really
    /// has. Nothing is sent while an apply still runs.
    void apply(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
    {
        if (dropEditIfTargetMoved(liveTarget) || !canApply(liveTarget) || applying())
        {
            return;
        }
        const Platform::ProcessTarget target = m_EditTarget;
        const Platform::IoPriority priority = m_Edit;
        m_Changed = false;
        m_EditTarget = NO_TARGET;
        if (actions == nullptr)
        {
            finishApply(Platform::ProcessActionResult::error("Process actions unavailable"));
            return;
        }
        m_Retry.reset();
        Platform::AuthorizedAction retry;
        retry.kind = Platform::AuthorizedAction::Kind::SetIoPriority;
        retry.target = target;
        retry.ioClass = priority.ioClass;
        retry.level = priority.level;
        static_cast<void>(m_ApplyRun.start(
            std::format("Setting the I/O priority of PID {}...", target.pid),
            [actions, target, priority] { return actions->setIoPriority(target, priority.ioClass, priority.level); },
            [this, generation = m_Generation, actions, retry](const Platform::ProcessActionResult& result)
            {
                if (generation == m_Generation)
                {
                    finishApply(result);
                    if (result.permissionDenied && actions->actionCapabilities().canAuthorizeInteractively)
                    {
                        m_Retry = Retry{.action = retry, .actions = actions};
                    }
                }
            }));
    }

    /// Whether the error line offers "Authenticate and retry" (#2308), as ProcessPriorityView's does.
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
            std::format("Waiting for authorization to set the I/O priority of PID {}...", retry.action.target.pid),
            [actions = retry.actions, action = retry.action](const std::stop_token& stopToken)
            { return actions->runAuthorized(action, stopToken); },
            [this, generation = m_Generation](const Platform::ProcessActionResult& result)
            {
                if (generation == m_Generation)
                {
                    finishApply(result); // a refusal to the administrator isn't offered again
                }
            }));
    }

    /// Each frame, before drawing: a finished apply's result goes to the error line. @p wait blocks
    /// until a running apply finishes (the tests' way to take it in deterministically).
    void update(bool wait = false)
    {
        m_ApplyRun.update(wait);
    }

    /// Whether an apply runs on the worker now: Apply is disabled and the line under it says so.
    [[nodiscard]] bool applying() const noexcept
    {
        return m_ApplyRun.busy();
    }

    /// What the line under Apply says while applying().
    [[nodiscard]] const std::string& applyingText() const noexcept
    {
        return m_ApplyRun.pendingText();
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

    /// A permission refusal the platform can ask to redo (#2308), through the panel's actions (which
    /// outlive this view).
    struct Retry
    {
        Platform::AuthorizedAction action;
        Platform::IProcessActions* actions = nullptr;
    };

    /// An apply's result, on the UI thread.
    void finishApply(const Platform::ProcessActionResult& result)
    {
        m_Read.reset(); // Read what the process has now, whichever way it went
        if (result.success)
        {
            m_Error.clear();
            return;
        }
        m_Error = result.errorMessage; // Stays until the next edit, apply or selection change
        if (!m_Changed)
        {
            m_Edit = m_Current.value_or(Platform::IoPriority{});
        }
    }

    /// The result shown for a read that threw, or one no worker could be started for.
    [[nodiscard]] static Platform::IoPriorityReadResult failedRead(std::string detail)
    {
        return std::unexpected(std::move(detail));
    }

    /// Whether m_Current was read for exactly @p liveTarget.
    /// Whether the read of exactly @p liveTarget was refused for want of access, and Apply may go ahead anyway.
    [[nodiscard]] bool readDeniedFor(const Platform::ProcessTarget& liveTarget) const noexcept
    {
        return m_ApplyWhenReadDenied && !m_Current.has_value() && m_CurrentTarget.pid == liveTarget.pid &&
               m_CurrentTarget.startTimeTicks == liveTarget.startTimeTicks && Platform::isIoPriorityReadDenied(m_ReadError);
    }

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
    bool m_ApplyWhenReadDenied = false; // allowApplyWhenReadDenied()'s
    std::uint64_t m_Generation = 0;     // Bumped by a selection change: a running apply's result is then dropped
    std::optional<Retry> m_Retry;
    // The current value's read and its cadence. Destroying the view waits for at most one read, which
    // touches only the panel's actions.
    Detail::LazyBackgroundRead<Platform::IoPriorityReadResult> m_Read{
        Domain::Sampling::PROCESS_IO_PRIORITY_REFRESH_MS, Platform::IO_PRIORITY_READ_THREAD_NAME, &failedRead};
    // Last, so it is destroyed (waiting for a running apply) before the state its finish writes.
    ProcessActionRun m_ApplyRun;
};

} // namespace App
