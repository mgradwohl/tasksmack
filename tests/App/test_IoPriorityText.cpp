// How an I/O priority is worded and ordered (#3551): the I/O Priority column and Process Details share the words, and
// the column sorts by the kernel's order of service.

#include "App/Panels/IoPriorityText.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace App::Detail
{
namespace
{

using Platform::IoPriority;
using Platform::IoPriorityClass;
using Platform::IoPriorityHint;

[[nodiscard]] std::string_view words(IoPriority priority, std::optional<std::int32_t> nice = std::nullopt)
{
    static thread_local IoPriorityText text; // keeps the view alive for the expectation
    text = ioPriorityText(priority, nice);
    return text.view();
}

TEST(IoPriorityTextTest, ItSaysWhatIoniceAndWindowsSay)
{
    EXPECT_EQ(words({.ioClass = IoPriorityClass::Realtime, .level = 0}), "realtime 0");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::BestEffort, .level = 4}), "best-effort 4");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::Idle, .level = 0}), "idle");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::None, .level = 0}, 19), "default (best-effort 7 from nice)");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::None, .level = 0}, 0), "default (best-effort 4 from nice)");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::None, .level = 0}), "default (from nice)");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(IoPriorityHint::VeryLow)}), "Very low");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(IoPriorityHint::Critical)}), "Critical");
    EXPECT_EQ(words({.ioClass = IoPriorityClass::WindowsHint, .level = 9}), "unknown (9)");
}

// Highest-served first: real time, then best-effort (a class never set at the level its nice derives), then idle;
// Windows' hints from Critical down; unknown after all of them.
TEST(IoPriorityTextTest, TheRankFollowsTheKernelsOrderOfService)
{
    const auto rank = [](std::optional<IoPriority> priority, std::int32_t nice = 0)
    {
        return ioPriorityRank(priority, nice);
    };
    const std::vector<std::int32_t> linux{
        rank(IoPriority{.ioClass = IoPriorityClass::Realtime, .level = 0}),
        rank(IoPriority{.ioClass = IoPriorityClass::Realtime, .level = 7}),
        rank(IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 0}),
        rank(IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 3}),
        rank(IoPriority{.ioClass = IoPriorityClass::None, .level = 0}, 0), // best-effort 4 from nice 0
        rank(IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 5}),
        rank(IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 7}),
        rank(IoPriority{.ioClass = IoPriorityClass::Idle, .level = 0}),
        rank(std::nullopt),
    };
    EXPECT_TRUE(std::ranges::is_sorted(linux));
    EXPECT_EQ(rank(IoPriority{.ioClass = IoPriorityClass::None, .level = 0}, 0),
              rank(IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 4}))
        << "a class never set is served as the best-effort level its nice gives";

    const auto hint = [&rank](IoPriorityHint h)
    {
        return rank(IoPriority{.ioClass = IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(h)});
    };
    EXPECT_TRUE(std::ranges::is_sorted(std::vector<std::int32_t>{hint(IoPriorityHint::Critical),
                                                                 hint(IoPriorityHint::High),
                                                                 hint(IoPriorityHint::Normal),
                                                                 hint(IoPriorityHint::Low),
                                                                 hint(IoPriorityHint::VeryLow),
                                                                 rank(std::nullopt)}));
}

} // namespace
} // namespace App::Detail
