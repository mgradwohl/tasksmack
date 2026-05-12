#include "UI/Theme.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <set>
#include <type_traits>

namespace UI
{
namespace
{

TEST(ThemeHeaderTest, FontSizeArrayMatchesEnumCount)
{
    using Underlying = std::underlying_type_t<FontSize>;
    constexpr auto enumCount = static_cast<std::size_t>(static_cast<Underlying>(FontSize::Count));

    EXPECT_EQ(ALL_FONT_SIZES.size(), enumCount);
    EXPECT_EQ(FONT_SIZE_COUNT, enumCount);
}

TEST(ThemeHeaderTest, FontSizeArrayContainsUniqueEntries)
{
    std::set<FontSize> uniqueSizes;
    for (const auto size : ALL_FONT_SIZES)
    {
        uniqueSizes.insert(size);
    }

    EXPECT_EQ(uniqueSizes.size(), ALL_FONT_SIZES.size());
}

TEST(ThemeHeaderTest, HexToImVec4ConvertsExpectedChannels)
{
    const ImVec4 color = hexToImVec4(0xFF8040);
    EXPECT_NEAR(color.x, 1.0F, 1e-6F);
    EXPECT_NEAR(color.y, 128.0F / 255.0F, 1e-6F);
    EXPECT_NEAR(color.z, 64.0F / 255.0F, 1e-6F);
    EXPECT_NEAR(color.w, 1.0F, 1e-6F);
}

TEST(ThemeHeaderTest, WithAlphaReturnsColorWithUpdatedAlpha)
{
    const ImVec4 original{0.1F, 0.2F, 0.3F, 0.4F};
    const ImVec4 updated = withAlpha(original, 0.85F);

    EXPECT_NEAR(updated.x, original.x, 1e-6F);
    EXPECT_NEAR(updated.y, original.y, 1e-6F);
    EXPECT_NEAR(updated.z, original.z, 1e-6F);
    EXPECT_NEAR(updated.w, 0.85F, 1e-6F);
}

} // namespace
} // namespace UI
