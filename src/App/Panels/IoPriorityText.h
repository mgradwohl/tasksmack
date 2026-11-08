#pragma once

// How an I/O priority is worded and ordered (#803, #3362, #3551): ionice(1)'s words on Linux ("best-effort 4", "idle",
// "default (best-effort 4 from nice)"), the hint's name on Windows ("Very low" to "High", and "Critical"). Process
// Details' header and the Processes table's I/O Priority column share them; the column draws every frame, so the text
// is built in a fixed buffer, without allocating.

#include "Domain/PriorityConfig.h"
#include "Platform/IoPriorityTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace App::Detail
{

/// A Windows I/O priority hint's name (#3362): "Very low", "Low", "Normal", "High", "Critical".
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

/// An I/O priority's words, in a fixed buffer (ioPriorityText()).
class IoPriorityText
{
  public:
    [[nodiscard]] std::string_view view() const noexcept
    {
        return {m_Chars.data(), m_Size};
    }

  private:
    friend IoPriorityText ioPriorityText(const Platform::IoPriority& priority, std::optional<std::int32_t> nice);

    template<typename... Args> void format(std::format_string<Args...> pattern, Args&&... args)
    {
        constexpr auto CAPACITY = static_cast<std::ptrdiff_t>(std::tuple_size_v<decltype(m_Chars)>);
        const auto result = std::format_to_n(m_Chars.data(), CAPACITY, pattern, std::forward<Args>(args)...);
        m_Size = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(result.size, 0, CAPACITY));
    }

    std::array<char, 40> m_Chars{};
    std::size_t m_Size = 0;
};

/// The words for @p priority: "realtime 0", "best-effort 4", "idle"; for a class never set "default (best-effort N
/// from nice)", N being the level the kernel derives from @p nice (just "default (from nice)" while it isn't known); a
/// Windows hint's name, or "unknown (N)" for a hint Windows doesn't define.
[[nodiscard]] inline IoPriorityText ioPriorityText(const Platform::IoPriority& priority, std::optional<std::int32_t> nice)
{
    IoPriorityText text;
    switch (priority.ioClass)
    {
    case Platform::IoPriorityClass::WindowsHint:
        if (const std::optional<Platform::IoPriorityHint> hint = Platform::ioPriorityHintFromLevel(priority.level); hint.has_value())
        {
            text.format("{}", ioPriorityHintName(*hint));
        }
        else
        {
            text.format("unknown ({})", priority.level);
        }
        break;
    case Platform::IoPriorityClass::Realtime:
        text.format("realtime {}", priority.level);
        break;
    case Platform::IoPriorityClass::BestEffort:
        text.format("best-effort {}", priority.level);
        break;
    case Platform::IoPriorityClass::Idle:
        text.format("idle");
        break;
    case Platform::IoPriorityClass::None:
    default:
        if (nice.has_value())
        {
            text.format("default (best-effort {} from nice)", Domain::Priority::ioLevelForNice(*nice));
        }
        else
        {
            text.format("default (from nice)");
        }
        break;
    }
    return text;
}

/// Where @p priority sorts, lower first being served first: real time 0-7, then best-effort 0-7 (a class never set at
/// the level @p nice derives, as the kernel serves it), then idle; Windows' hints from Critical to Very low. An
/// unknown I/O priority sorts after every known one.
[[nodiscard]] constexpr std::int32_t ioPriorityRank(const std::optional<Platform::IoPriority>& priority, std::int32_t nice) noexcept
{
    constexpr std::int32_t LEVELS = 8;
    if (!priority.has_value())
    {
        return std::numeric_limits<std::int32_t>::max();
    }
    switch (priority->ioClass)
    {
    case Platform::IoPriorityClass::Realtime:
        return priority->level;
    case Platform::IoPriorityClass::BestEffort:
        return LEVELS + priority->level;
    case Platform::IoPriorityClass::None:
        return LEVELS + Domain::Priority::ioLevelForNice(nice);
    case Platform::IoPriorityClass::Idle:
        return 2 * LEVELS;
    case Platform::IoPriorityClass::WindowsHint:
        // Higher hints are more urgent: Critical (4) first.
        return (3 * LEVELS) + (static_cast<std::int32_t>(Platform::IoPriorityHint::Critical) - priority->level);
    }
    return std::numeric_limits<std::int32_t>::max() - 1;
}

} // namespace App::Detail
