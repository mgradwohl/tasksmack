/// @file test_ColorDifference.cpp
/// @brief CIELAB, CIEDE2000 and dichromacy simulation used to check chart series separation (#1197).

#include "UI/ColorDifference.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <array>

namespace UI::ColorDifference
{
namespace
{

constexpr double REFERENCE_TOLERANCE = 1e-4;

struct ReferencePair
{
    Lab first;
    Lab second;
    double expected = 0.0;
};

// Pairs from Sharma, Wu and Dalal (2005), Table 1, chosen to exercise the formula's awkward branches:
// near-neutral pairs across the a* axis (the hue mean wraps), the blue-region rotation term, and large
// differences in all three components.
constexpr std::array SHARMA_PAIRS{
    ReferencePair{.first = {.l = 50.0, .a = 2.6772, .b = -79.7751}, .second = {.l = 50.0, .a = 0.0, .b = -82.7485}, .expected = 2.0425},
    ReferencePair{.first = {.l = 50.0, .a = 3.1571, .b = -77.2803}, .second = {.l = 50.0, .a = 0.0, .b = -82.7485}, .expected = 2.8615},
    ReferencePair{.first = {.l = 50.0, .a = 0.0, .b = 0.0}, .second = {.l = 50.0, .a = -1.0, .b = 2.0}, .expected = 2.3669},
    ReferencePair{.first = {.l = 50.0, .a = 2.49, .b = -0.001}, .second = {.l = 50.0, .a = -2.49, .b = 0.0009}, .expected = 7.1792},
    ReferencePair{.first = {.l = 50.0, .a = 2.49, .b = -0.001}, .second = {.l = 50.0, .a = -2.49, .b = 0.0011}, .expected = 7.2195},
    ReferencePair{.first = {.l = 50.0, .a = 2.5, .b = 0.0}, .second = {.l = 73.0, .a = 25.0, .b = -18.0}, .expected = 27.1492},
    ReferencePair{.first = {.l = 50.0, .a = 2.5, .b = 0.0}, .second = {.l = 56.0, .a = -27.0, .b = -3.0}, .expected = 31.9030},
    ReferencePair{.first = {.l = 50.0, .a = 2.5, .b = 0.0}, .second = {.l = 58.0, .a = 24.0, .b = 15.0}, .expected = 19.4535},
    ReferencePair{
        .first = {.l = 60.2574, .a = -34.0099, .b = 36.2677},
        .second = {.l = 60.4626, .a = -34.1751, .b = 39.4387},
        .expected = 1.2644,
    },
    ReferencePair{
        .first = {.l = 22.7233, .a = 20.0904, .b = -46.6940},
        .second = {.l = 23.0331, .a = 14.9730, .b = -42.5619},
        .expected = 2.0373,
    },
};

auto rgb(unsigned hex) -> ImVec4
{
    return {static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(hex & 0xFFU) / 255.0F,
            1.0F};
}

TEST(ColorDifferenceTest, MatchesSharmaReferenceData)
{
    for (const auto& pair : SHARMA_PAIRS)
    {
        EXPECT_NEAR(deltaE2000(pair.first, pair.second), pair.expected, REFERENCE_TOLERANCE)
            << "L*a*b* (" << pair.first.l << ", " << pair.first.a << ", " << pair.first.b << ") vs (" << pair.second.l << ", "
            << pair.second.a << ", " << pair.second.b << ")";
        // The formula is symmetric.
        EXPECT_NEAR(deltaE2000(pair.second, pair.first), pair.expected, REFERENCE_TOLERANCE);
    }
}

TEST(ColorDifferenceTest, IdenticalColoursHaveNoDifference)
{
    EXPECT_DOUBLE_EQ(deltaE2000(rgb(0xEC407A), rgb(0xEC407A)), 0.0);
    EXPECT_DOUBLE_EQ(deltaE2000(Lab{}, Lab{}), 0.0);
}

TEST(ColorDifferenceTest, ConvertsSrgbToLab)
{
    const Lab white = toLab(rgb(0xFFFFFF));
    EXPECT_NEAR(white.l, 100.0, 1e-3);
    EXPECT_NEAR(white.a, 0.0, 1e-3);
    EXPECT_NEAR(white.b, 0.0, 1e-3);

    const Lab black = toLab(rgb(0x000000));
    EXPECT_NEAR(black.l, 0.0, 1e-9);

    // sRGB red, D65: the commonly quoted (53.24, 80.09, 67.20).
    const Lab red = toLab(rgb(0xFF0000));
    EXPECT_NEAR(red.l, 53.24, 0.01);
    EXPECT_NEAR(red.a, 80.09, 0.01);
    EXPECT_NEAR(red.b, 67.20, 0.01);
}

TEST(ColorDifferenceTest, SimulationLeavesGreysAndAlphaAlone)
{
    const ImVec4 grey{0.5F, 0.5F, 0.5F, 0.25F};
    for (const Deficiency deficiency : {Deficiency::Protanopia, Deficiency::Deuteranopia})
    {
        const ImVec4 seen = simulate(grey, deficiency);
        EXPECT_NEAR(seen.x, 0.5F, 1e-3F);
        EXPECT_NEAR(seen.y, 0.5F, 1e-3F);
        EXPECT_NEAR(seen.z, 0.5F, 1e-3F);
        EXPECT_FLOAT_EQ(seen.w, 0.25F);
    }
}

// The classic failure: a red and a green that are far apart for most viewers collapse for dichromats,
// while a blue and a yellow stay apart.
TEST(ColorDifferenceTest, RedAndGreenCollapseUnderSimulationButBlueAndYellowDoNot)
{
    const ImVec4 red = rgb(0xD32F2F);
    const ImVec4 green = rgb(0x388E3C);
    EXPECT_GT(deltaE2000(red, green), 60.0);
    EXPECT_LT(deltaE2000As(red, green, Deficiency::Protanopia), 20.0);
    EXPECT_LT(deltaE2000As(red, green, Deficiency::Deuteranopia), 8.0);

    const ImVec4 blue = rgb(0x1E88E5);
    const ImVec4 yellow = rgb(0xFDD835);
    EXPECT_GT(deltaE2000As(blue, yellow, Deficiency::Protanopia), 50.0);
    EXPECT_GT(deltaE2000As(blue, yellow, Deficiency::Deuteranopia), 50.0);
}

// #1252 review: the palette gate's protan/deutan floors are only as good as these coefficients, so
// pin the simulation to exact outputs of Machado 2009's severity-1.0 matrices. The expected sRGB
// values were computed independently: sRGB -> linear (IEC 61966-2-1), the published matrix, back
// to sRGB clamped to the gamut. A wrong coefficient moves at least one of these.
TEST(ColorDifferenceTest, SimulationMatchesMachadoReferenceOutputs)
{
    struct Case
    {
        ImVec4 input;
        Deficiency deficiency;
        std::array<float, 3> expected;
    };
    const std::array cases{
        Case{.input = {1.0F, 0.0F, 0.0F, 1.0F}, .deficiency = Deficiency::Protanopia, .expected = {0.426608F, 0.372654F, 0.0F}},
        Case{.input = {0.0F, 1.0F, 0.0F, 1.0F}, .deficiency = Deficiency::Protanopia, .expected = {1.0F, 0.899428F, 0.0F}},
        Case{.input = {0.8F, 0.4F, 0.2F, 1.0F}, .deficiency = Deficiency::Protanopia, .expected = {0.511696F, 0.457628F, 0.175910F}},
        Case{.input = {1.0F, 0.0F, 0.0F, 1.0F}, .deficiency = Deficiency::Deuteranopia, .expected = {0.640060F, 0.565807F, 0.0F}},
        Case{.input = {0.0F, 1.0F, 0.0F, 1.0F}, .deficiency = Deficiency::Deuteranopia, .expected = {0.936051F, 0.839248F, 0.229192F}},
        Case{.input = {0.8F, 0.4F, 0.2F, 1.0F}, .deficiency = Deficiency::Deuteranopia, .expected = {0.608539F, 0.546899F, 0.191920F}},
    };
    for (const Case& c : cases)
    {
        const ImVec4 seen = simulate(c.input, c.deficiency);
        EXPECT_NEAR(seen.x, c.expected[0], 1e-5F);
        EXPECT_NEAR(seen.y, c.expected[1], 1e-5F);
        EXPECT_NEAR(seen.z, c.expected[2], 1e-5F);
    }
}

TEST(ColorDifferenceTest, OutOfRangeInputIsClamped)
{
    const ImVec4 overWhite{2.0F, 2.0F, 2.0F, 1.0F};
    EXPECT_NEAR(deltaE2000(overWhite, rgb(0xFFFFFF)), 0.0, 1e-9);
}

// OKLab of the sRGB primaries, from Ottosson's reference implementation, as OKLCH (#1196).
TEST(ColorDifferenceTest, OklchMatchesOttossonReferenceValues)
{
    constexpr double TOLERANCE = 1e-3;
    struct Reference
    {
        ImVec4 color;
        Oklch expected;
    };
    const std::array references{
        Reference{.color = {1.0F, 0.0F, 0.0F, 1.0F}, .expected = {.l = 0.627955, .c = 0.257683, .h = 29.2339}},
        Reference{.color = {0.0F, 1.0F, 0.0F, 1.0F}, .expected = {.l = 0.866440, .c = 0.294827, .h = 142.4953}},
        Reference{.color = {0.0F, 0.0F, 1.0F, 1.0F}, .expected = {.l = 0.452014, .c = 0.313214, .h = 264.0520}},
    };
    for (const auto& [color, expected] : references)
    {
        const Oklch actual = toOklch(color);
        EXPECT_NEAR(actual.l, expected.l, TOLERANCE);
        EXPECT_NEAR(actual.c, expected.c, TOLERANCE);
        EXPECT_NEAR(actual.h, expected.h, 0.05);
    }

    const Oklch white = toOklch({1.0F, 1.0F, 1.0F, 1.0F});
    EXPECT_NEAR(white.l, 1.0, TOLERANCE);
    EXPECT_NEAR(white.c, 0.0, TOLERANCE);
}

} // namespace
} // namespace UI::ColorDifference
