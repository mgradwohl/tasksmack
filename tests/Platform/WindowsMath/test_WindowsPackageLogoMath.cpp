/// @file test_WindowsPackageLogoMath.cpp
/// @brief Which file a packaged app's logo is read from (#1800): the variants of the manifest's logo
/// by scale and target size, the one nearest the size drawn (the unplated target size first among
/// equals), high-contrast and light-theme variants left out, other files ignored, and nothing when no
/// variant is there. Pure, so it runs on every platform.

#include "Platform/Windows/WindowsPackageLogoMath.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows::PackageLogo
{
namespace
{

constexpr const char* LOGO = "Square44x44Logo.png";

/// What variantOf() makes of @p candidate as a variant of LOGO; size and rank -1 when it is none.
[[nodiscard]] Variant variant(std::string_view candidate)
{
    return variantOf(LOGO, candidate).value_or(Variant{.sizePx = -1, .rank = -1});
}

/// The name pickLogo() picks among @p names for @p sizePx, or "" for none.
[[nodiscard]] std::string picked(const std::vector<std::string>& names, int sizePx, const char* logo = LOGO)
{
    const std::optional<std::size_t> index = pickLogo(logo, names, sizePx);
    return index.has_value() ? names[*index] : std::string();
}

TEST(WindowsPackageLogoMathTest, TheNominalSizeComesFromTheLogosName)
{
    EXPECT_EQ(nominalSize("square44x44logo"), 44);
    EXPECT_EQ(nominalSize("Square150x150Logo"), 150);
    EXPECT_EQ(nominalSize("Logo1x"), 1);
    EXPECT_EQ(nominalSize("StoreLogo"), 44); // No size in the name: Square44x44Logo's
    EXPECT_EQ(nominalSize("App2Logo"), 44);  // A number not followed by x is no size
}

TEST(WindowsPackageLogoMathTest, VariantsAreToldBySizeAndPlate)
{
    const Variant unplated = variant("Square44x44Logo.targetsize-24_altform-unplated.png");
    EXPECT_EQ(unplated.sizePx, 24);
    EXPECT_EQ(unplated.rank, 0);

    const Variant plated = variant("Square44x44Logo.targetsize-32.png");
    EXPECT_EQ(plated.sizePx, 32);
    EXPECT_EQ(plated.rank, 1);

    const Variant scaled = variant("Square44x44Logo.scale-200.png");
    EXPECT_EQ(scaled.sizePx, 88);
    EXPECT_EQ(scaled.rank, 2);

    const Variant base = variant("Square44x44Logo.png");
    EXPECT_EQ(base.sizePx, 44);
    EXPECT_EQ(base.rank, 2);

    // The qualifiers in either order, and any letter case.
    const Variant reordered = variant("SQUARE44X44LOGO.ALTFORM-UNPLATED_TARGETSIZE-16.PNG");
    EXPECT_EQ(reordered.sizePx, 16);
    EXPECT_EQ(reordered.rank, 0);
}

TEST(WindowsPackageLogoMathTest, VariantsTaskSmackDoesNotDrawAndOtherFilesAreNotVariants)
{
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.scale-100_contrast-black.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.targetsize-16_altform-lightunplated.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.targetsize-16_contrast-white.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.theme-dark_scale-100.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.altform-unplated.png").has_value()); // No size
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.scale-.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.targetsize-0.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.scale-100_.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.targetsize-99999.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square150x150Logo.scale-100.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo.scale-100.jpg").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44LogoX.scale-100.png").has_value());
    EXPECT_FALSE(variantOf(LOGO, "Square44x44Logo..png").has_value());
    EXPECT_FALSE(variantOf("NoExtension", "NoExtension.scale-100").has_value());
}

TEST(WindowsPackageLogoMathTest, TheSmallestVariantAtLeastTheSizeIsPicked)
{
    const std::vector<std::string> names{
        "Square44x44Logo.targetsize-16_altform-unplated.png",
        "Square44x44Logo.targetsize-32_altform-unplated.png",
        "Square44x44Logo.targetsize-48_altform-unplated.png",
        "Square44x44Logo.targetsize-256_altform-unplated.png",
    };
    EXPECT_EQ(picked(names, 16), names[0]);
    EXPECT_EQ(picked(names, 20), names[1]); // Shrunk from 32, not grown from 16
    EXPECT_EQ(picked(names, 32), names[1]);
    EXPECT_EQ(picked(names, 40), names[2]);
    EXPECT_EQ(picked(names, 300), names[3]); // None large enough: the largest
}

TEST(WindowsPackageLogoMathTest, AmongOneSizeTheUnplatedTargetSizeComesFirst)
{
    const std::vector<std::string> names{
        "Square44x44Logo.scale-100.png",                     // 44
        "Square44x44Logo.targetsize-44.png",                 // 44, plated
        "Square44x44Logo.targetsize-44_altform-unplated.png" // 44, unplated
    };
    EXPECT_EQ(picked(names, 40), names[2]);

    const std::vector<std::string> platedOrScaled{"Square44x44Logo.scale-100.png", "Square44x44Logo.targetsize-44.png"};
    EXPECT_EQ(picked(platedOrScaled, 40), platedOrScaled[1]);
}

TEST(WindowsPackageLogoMathTest, ScaleVariantsAreSizedFromTheLogosName)
{
    const std::vector<std::string> names{
        "Square44x44Logo.scale-100.png", // 44
        "Square44x44Logo.scale-200.png", // 88
        "Square44x44Logo.scale-400.png", // 176
    };
    EXPECT_EQ(picked(names, 24), names[0]);
    EXPECT_EQ(picked(names, 64), names[1]);
    EXPECT_EQ(picked(names, 128), names[2]);

    const std::vector<std::string> large{"Square150x150Logo.scale-100.png", "Square150x150Logo.scale-50.png"};
    EXPECT_EQ(picked(large, 32, "Square150x150Logo.png"), large[1]); // 75 px, before 150
}

TEST(WindowsPackageLogoMathTest, HighContrastAndUnrelatedFilesAreNeverPicked)
{
    const std::vector<std::string> names{
        "Wide310x150Logo.scale-100.png",
        "Square44x44Logo.targetsize-16_contrast-black.png",
        "Square44x44Logo.targetsize-16_altform-lightunplated.png",
        "Square44x44Logo.scale-200.png",
    };
    EXPECT_EQ(picked(names, 16), names[3]);
}

TEST(WindowsPackageLogoMathTest, NoVariantIsNoPick)
{
    EXPECT_FALSE(pickLogo(LOGO, std::vector<std::string>{}, 16).has_value());
    EXPECT_FALSE(pickLogo(LOGO, std::vector<std::string>{"SplashScreen.scale-100.png", "Square44x44Logo.scale-100_contrast-black.png"}, 16)
                     .has_value());
}

TEST(WindowsPackageLogoMathTest, TheUnqualifiedLogoIsPickedWhenItIsAllThereIs)
{
    const std::vector<std::string> names{"StoreLogo.png", "Other.png"};
    EXPECT_EQ(picked(names, 16, "StoreLogo.png"), names[0]);
}

} // namespace
} // namespace Platform::Windows::PackageLogo
