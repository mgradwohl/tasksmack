/// @file test_ProcessTypeColor.cpp
/// @brief Tests for App::processTypeColor(), shared by the Type column and Process Details (#1180)

#include "App/Panels/ProcessTypeColor.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <array>
#include <string_view>

namespace
{

/// A scheme whose type colours are all distinct, so a test can tell which one was chosen.
UI::ColorScheme distinctScheme()
{
    UI::ColorScheme scheme{};
    scheme.statusRunning = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);
    scheme.textInfo = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    scheme.textMuted = ImVec4(0.0F, 0.0F, 1.0F, 1.0F);
    return scheme;
}

void expectSameColor(const ImVec4& actual, const ImVec4& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.z, expected.z);
    EXPECT_FLOAT_EQ(actual.w, expected.w);
}

TEST(ProcessTypeColorTest, EveryKnownTypeGetsItsColour)
{
    const UI::ColorScheme scheme = distinctScheme();
    struct Case
    {
        std::string_view type;
        ImVec4 expected;
    };
    const std::array<Case, 3> cases{{
        {.type = "App", .expected = scheme.statusRunning},
        {.type = "Windows Process", .expected = scheme.textInfo},
        {.type = "Background Process", .expected = scheme.textMuted},
    }};

    for (const auto& c : cases)
    {
        SCOPED_TRACE(c.type);
        expectSameColor(App::processTypeColor(c.type, scheme), c.expected);
    }
}

TEST(ProcessTypeColorTest, UnknownTypesAreMuted)
{
    const UI::ColorScheme scheme = distinctScheme();
    for (const std::string_view type : {"", "app", "APP", "App ", "Service", "Windows process"})
    {
        SCOPED_TRACE(type);
        expectSameColor(App::processTypeColor(type, scheme), scheme.textMuted);
    }
}

} // namespace
