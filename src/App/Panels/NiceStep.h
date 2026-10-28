#pragma once

// F7 and F8 on the Processes table (#3097), htop's "nice -" and "nice +": the selected process's
// priority one step higher or lower, applied at once. One step is one nice value on Linux and one
// priority class on Windows (Idle to High; F7 never reaches Realtime). Pure, so tested on every
// platform without ImGui (test_NiceStep.cpp).

#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h"
#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>

namespace App::NiceStep
{

/// Which way a key moves the priority.
enum class Direction : std::uint8_t
{
    Raise, ///< F7: a higher priority (a lower nice value)
    Lower, ///< F8: a lower priority (a higher nice value)
};

/// The nice value one step from a process at @p nice (and @p priorityClass, Windows) in @p direction;
/// nullopt at the end of the range. With @p windowsClasses the step is to the next class's classNice().
[[nodiscard]] constexpr std::optional<std::int32_t> next(std::int32_t nice,
                                                         Domain::Priority::PriorityClass priorityClass,
                                                         Direction direction,
                                                         bool windowsClasses = Detail::PRIORITY_USES_WINDOWS_CLASSES) noexcept
{
    namespace P = Domain::Priority;
    if (!windowsClasses)
    {
        const std::int32_t stepped = nice + (direction == Direction::Raise ? -1 : 1);
        return (stepped < P::MIN_NICE || stepped > P::MAX_NICE) ? std::nullopt : std::optional<std::int32_t>(stepped);
    }
    // The classes F7 and F8 move through, lowest first.
    constexpr std::array<P::PriorityClass, 5> CLASSES{P::PriorityClass::Idle,
                                                      P::PriorityClass::BelowNormal,
                                                      P::PriorityClass::Normal,
                                                      P::PriorityClass::AboveNormal,
                                                      P::PriorityClass::High};
    const P::PriorityClass current = priorityClass != P::PriorityClass::None ? priorityClass : P::classFromNice(nice);
    if (current == P::PriorityClass::Realtime)
    {
        return direction == Direction::Lower ? std::optional<std::int32_t>(P::classNice(P::PriorityClass::High)) : std::nullopt;
    }
    // An index, not the iterator itself: std::array's iterator is a pointer in libc++ and a class in MSVC's STL.
    const auto index = static_cast<std::size_t>(std::ranges::distance(CLASSES.begin(), std::ranges::find(CLASSES, current)));
    if (index >= CLASSES.size())
    {
        return std::nullopt;
    }
    const auto at = [&CLASSES](std::size_t i)
    {
        return P::classNice(*std::next(CLASSES.begin(), static_cast<std::ptrdiff_t>(i)));
    };
    if (direction == Direction::Raise)
    {
        return index + 1 < CLASSES.size() ? std::optional<std::int32_t>(at(index + 1)) : std::nullopt;
    }
    return index > 0 ? std::optional<std::int32_t>(at(index - 1)) : std::nullopt;
}

/// The toolbar's line for a step: "Priority of foo (PID 42) set to Below Normal (nice: 10)", or the
/// platform's error.
[[nodiscard]] inline Detail::ActionResultMessage resultMessage(const std::string& name,
                                                               std::int32_t pid,
                                                               std::int32_t nice,
                                                               const Platform::ProcessActionResult& result,
                                                               bool windowsClasses = Detail::PRIORITY_USES_WINDOWS_CLASSES)
{
    if (!result.success)
    {
        return {.ok = false, .text = result.errorMessage};
    }
    return {.ok = true,
            .text = std::format("Priority of {} (PID {}) set to {}", name, pid, ProcessBatch::priorityValueText(nice, windowsClasses))};
}

} // namespace App::NiceStep
