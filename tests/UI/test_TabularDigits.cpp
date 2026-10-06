#include "UI/TabularDigits.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <vector>

namespace UI
{
namespace
{

/// The bundled body font, next to the themes the tests already find in the source tree.
[[nodiscard]] std::vector<std::byte> readInterRegular()
{
    const auto path = std::filesystem::path(TASKSMACK_SOURCE_THEMES_DIR).parent_path() / "fonts" / "Inter-Regular.ttf";
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

TEST(TabularDigitsTest, InterHasAPlausibleWidestDigit)
{
    const auto font = readInterRegular();
    ASSERT_FALSE(font.empty()) << "Inter-Regular.ttf not found";
    const std::optional<float> ratio = widestDigitAdvanceRatio(font);
    ASSERT_TRUE(ratio.has_value());
    // A digit is narrower than the line it sits on, and far wider than nothing.
    const float widest = ratio.value_or(0.0F);
    EXPECT_GT(widest, 0.3F);
    EXPECT_LT(widest, 1.0F);
}

TEST(TabularDigitsTest, UnreadableFontDataHasNoRatio)
{
    EXPECT_FALSE(widestDigitAdvanceRatio({}).has_value());
    const std::vector<std::byte> junk(256, std::byte{0x42});
    EXPECT_FALSE(widestDigitAdvanceRatio(junk).has_value());
}

TEST(TabularDigitsTest, AdvanceIsRoundedUpToAWholePixel)
{
    EXPECT_FLOAT_EQ(tabularDigitAdvancePx(0.5F, 20.0F), 10.0F);
    EXPECT_FLOAT_EQ(tabularDigitAdvancePx(0.51F, 20.0F), 11.0F); // 10.2 -> 11, never narrower than a digit
    EXPECT_FLOAT_EQ(tabularDigitAdvancePx(0.0F, 20.0F), 0.0F);
    EXPECT_FLOAT_EQ(tabularDigitAdvancePx(0.5F, -1.0F), 0.0F);
    EXPECT_FLOAT_EQ(tabularDigitAdvancePx(std::numeric_limits<float>::quiet_NaN(), 20.0F), 0.0F);
}

TEST(TabularDigitsTest, ExcludeRangesLeaveOnlyTheDigits)
{
    // Pairs of inclusive ranges, zero-terminated: every codepoint outside them is a digit.
    const auto excluded = [](unsigned int codepoint)
    {
        for (std::size_t i = 0; i + 1 < NON_DIGIT_GLYPH_RANGES.size() && NON_DIGIT_GLYPH_RANGES[i] != 0; i += 2)
        {
            if (codepoint >= NON_DIGIT_GLYPH_RANGES[i] && codepoint <= NON_DIGIT_GLYPH_RANGES[i + 1])
            {
                return true;
            }
        }
        return false;
    };
    for (unsigned int c = '0'; c <= '9'; ++c)
    {
        EXPECT_FALSE(excluded(c)) << c;
    }
    EXPECT_TRUE(excluded('/'));
    EXPECT_TRUE(excluded(':'));
    EXPECT_TRUE(excluded('.'));
    EXPECT_TRUE(excluded('A'));
    EXPECT_TRUE(excluded(0x00B0)); // °
    EXPECT_TRUE(excluded(IM_UNICODE_CODEPOINT_MAX));
    EXPECT_EQ(NON_DIGIT_GLYPH_RANGES.back(), 0);
}

} // namespace
} // namespace UI
