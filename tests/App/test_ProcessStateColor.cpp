/// @file test_ProcessStateColor.cpp
/// @brief Tests for App::processStateColor(), shared by the State column and Process Details (#1352)

#include "App/Panels/ProcessStateColor.h"
#include "Domain/ProcessState.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <array>
#include <string>
#include <string_view>

namespace
{

/// A scheme whose state colours are all distinct, so a test can tell which one was chosen.
UI::ColorScheme distinctScheme()
{
    UI::ColorScheme scheme{};
    scheme.statusRunning = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);
    scheme.statusSleeping = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    scheme.statusDiskSleep = ImVec4(0.0F, 0.0F, 1.0F, 1.0F);
    scheme.statusZombie = ImVec4(1.0F, 1.0F, 0.0F, 1.0F);
    scheme.statusStopped = ImVec4(1.0F, 0.0F, 1.0F, 1.0F);
    scheme.statusIdle = ImVec4(0.0F, 1.0F, 1.0F, 1.0F);
    return scheme;
}

void expectSameColor(const ImVec4& actual, const ImVec4& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.z, expected.z);
    EXPECT_FLOAT_EQ(actual.w, expected.w);
}

TEST(ProcessStateColorTest, EveryDisplayStateGetsItsStateColour)
{
    const UI::ColorScheme scheme = distinctScheme();
    struct Case
    {
        std::string_view name;
        ImVec4 expected;
    };
    const std::array<Case, 9> cases{{
        {.name = "Running", .expected = scheme.statusRunning},
        {.name = "Sleeping", .expected = scheme.statusSleeping},
        {.name = "Disk Sleep", .expected = scheme.statusDiskSleep},
        {.name = "Zombie", .expected = scheme.statusZombie},
        {.name = "Stopped", .expected = scheme.statusStopped},
        {.name = "Tracing", .expected = scheme.statusStopped},
        {.name = "Dead", .expected = scheme.statusSleeping},
        {.name = "Idle", .expected = scheme.statusIdle},
        {.name = "Unknown", .expected = scheme.statusSleeping},
    }};

    for (const auto& c : cases)
    {
        SCOPED_TRACE(std::string(c.name));
        // Process Details colours by name; the State column colours by the code it prints. Both
        // must land on the same colour.
        expectSameColor(App::processStateColor(c.name, scheme), c.expected);
        expectSameColor(App::processStateColor(Domain::processStateCode(c.name), scheme), c.expected);
    }
}

TEST(ProcessStateColorTest, StoppedIsNotColouredAsSleepingAndDeadIsNotDiskSleep)
{
    // #1352: the column used to switch on the name's first letter, so Stopped took the sleeping
    // colour and Dead the disk-sleep colour.
    const UI::ColorScheme scheme = distinctScheme();
    expectSameColor(App::processStateColor("Stopped", scheme), scheme.statusStopped);
    expectSameColor(App::processStateColor("Dead", scheme), scheme.statusSleeping);
}

} // namespace
