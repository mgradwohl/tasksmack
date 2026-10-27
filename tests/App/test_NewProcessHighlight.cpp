// The Processes table's new-process tint (#3060).

#include "App/Panels/NewProcessHighlight.h"

#include <gtest/gtest.h>

#include <optional>

namespace App
{
namespace
{

TEST(NewProcessHighlightTest, ANewProcessIsTintedAndFadesOverTheDuration)
{
    constexpr double NOW = 1'000'000.0;
    const std::optional<float> justStarted = NewProcessHighlight::alpha(1'000'000, NOW);
    ASSERT_TRUE(justStarted.has_value());
    EXPECT_FLOAT_EQ(*justStarted, NewProcessHighlight::MAX_ALPHA);

    const std::optional<float> halfway = NewProcessHighlight::alpha(1'000'000, NOW + (NewProcessHighlight::DURATION_SECONDS / 2.0));
    ASSERT_TRUE(halfway.has_value());
    EXPECT_FLOAT_EQ(*halfway, NewProcessHighlight::MAX_ALPHA / 2.0F);

    EXPECT_FALSE(NewProcessHighlight::alpha(1'000'000, NOW + NewProcessHighlight::DURATION_SECONDS).has_value()) << "faded out";
    EXPECT_FALSE(NewProcessHighlight::alpha(999'000, NOW).has_value()) << "an old process";
}

TEST(NewProcessHighlightTest, AnUnknownStartIsNeverTintedAndAFutureOneCountsAsNow)
{
    EXPECT_FALSE(NewProcessHighlight::alpha(0, 1'000'000.0).has_value());
    const std::optional<float> early = NewProcessHighlight::alpha(1'000'001, 1'000'000.5);
    ASSERT_TRUE(early.has_value());
    EXPECT_FLOAT_EQ(*early, NewProcessHighlight::MAX_ALPHA);
}

} // namespace
} // namespace App
