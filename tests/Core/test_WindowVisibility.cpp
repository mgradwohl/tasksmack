/// @file test_WindowVisibility.cpp
/// @brief Tests for the pure decisions behind Window::isHidden() in Core/WindowVisibility.h (#3152):
/// which states count as hidden, how WM_WTSSESSION_CHANGE's wParam and a power-setting notification's
/// display state decode, how each changes the window's visibility signals, and when the frame loop
/// asks DWM again whether the window is cloaked. The Win32 values are mirrored there and checked
/// against <windows.h> in Window.cpp; these tests run on every platform.

#include "Core/WindowConstants.h"
#include "Core/WindowVisibility.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>

namespace Core::WindowVisibility
{
namespace
{

using namespace std::chrono_literals;

/// The four bytes of a DWORD in the machine's byte order, as a POWERBROADCAST_SETTING's Data holds it.
[[nodiscard]] auto dwordBytes(std::uint32_t value) -> std::array<std::byte, sizeof(std::uint32_t)>
{
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

// ---- isHidden ----

TEST(WindowVisibilityTest, AShownWindowIsNotHidden)
{
    EXPECT_FALSE(isHidden(false, false, Signals{}));
    EXPECT_FALSE(anyHides(Signals{}));
}

TEST(WindowVisibilityTest, MinimizedOrOccludedIsHidden)
{
    EXPECT_TRUE(isHidden(true, false, Signals{}));
    EXPECT_TRUE(isHidden(false, true, Signals{}));
    EXPECT_TRUE(isHidden(true, true, Signals{}));
}

TEST(WindowVisibilityTest, EachWindowsSignalHidesTheWindowOnItsOwn)
{
    const std::array signals{
        Signals{.cloaked = true},
        Signals{.sessionLocked = true},
        Signals{.sessionDisconnected = true},
        Signals{.displayOff = true},
    };
    for (const Signals& each : signals)
    {
        EXPECT_TRUE(anyHides(each));
        EXPECT_TRUE(isHidden(false, false, each));
        EXPECT_TRUE(isHidden(true, false, each));
    }
}

// ---- classifyHidden (#3156) ----

TEST(WindowVisibilityTest, AShownWindowClassifiesAsShown)
{
    EXPECT_EQ(classifyHidden(false, true, false, Signals{}), HiddenState::Shown);
    EXPECT_EQ(classifyHidden(false, false, false, Signals{}), HiddenState::Shown);
}

TEST(WindowVisibilityTest, AHiddenStateThatEndsWithAnEventClassifiesAsHidden)
{
    EXPECT_EQ(classifyHidden(true, true, false, Signals{}), HiddenState::Hidden);  // minimized, end reported
    EXPECT_EQ(classifyHidden(false, true, true, Signals{}), HiddenState::Hidden);  // occluded
    EXPECT_EQ(classifyHidden(false, false, true, Signals{}), HiddenState::Hidden); // occluded on native Wayland
    for (const Signals& each : {Signals{.sessionLocked = true}, Signals{.sessionDisconnected = true}, Signals{.displayOff = true}})
    {
        EXPECT_EQ(classifyHidden(false, true, false, each), HiddenState::Hidden);
        // Minimized on native Wayland too: the lock's end is still reported.
        EXPECT_EQ(classifyHidden(true, false, false, each), HiddenState::Hidden);
    }
}

TEST(WindowVisibilityTest, ACloakClassifiesAsCloakedWhateverElseHidesTheWindow)
{
    const Signals cloaked{.cloaked = true};
    EXPECT_EQ(classifyHidden(false, true, false, cloaked), HiddenState::Cloaked);
    EXPECT_EQ(classifyHidden(true, true, true, cloaked), HiddenState::Cloaked);
    EXPECT_EQ(classifyHidden(false, true, false, Signals{.cloaked = true, .sessionLocked = true}), HiddenState::Cloaked);
}

TEST(WindowVisibilityTest, ANativeWaylandMinimizeAloneIsUnconfirmed)
{
    EXPECT_EQ(classifyHidden(true, false, false, Signals{}), HiddenState::MinimizedUnconfirmed);
}

TEST(WindowVisibilityTest, ClassifyHiddenAgreesWithIsHidden)
{
    for (const bool minimized : {false, true})
    {
        for (const bool reported : {false, true})
        {
            for (const bool occluded : {false, true})
            {
                for (const Signals& each : {Signals{}, Signals{.cloaked = true}, Signals{.displayOff = true}})
                {
                    EXPECT_EQ(classifyHidden(minimized, reported, occluded, each) != HiddenState::Shown,
                              isHidden(minimized, occluded, each));
                }
            }
        }
    }
}

// ---- decodeSessionChange ----

TEST(WindowVisibilityTest, SessionChangesDecodeFromTheirWin32Values)
{
    EXPECT_EQ(decodeSessionChange(0x7U), SessionChange::Lock);       // WTS_SESSION_LOCK
    EXPECT_EQ(decodeSessionChange(0x8U), SessionChange::Unlock);     // WTS_SESSION_UNLOCK
    EXPECT_EQ(decodeSessionChange(0x1U), SessionChange::Connect);    // WTS_CONSOLE_CONNECT
    EXPECT_EQ(decodeSessionChange(0x3U), SessionChange::Connect);    // WTS_REMOTE_CONNECT
    EXPECT_EQ(decodeSessionChange(0x2U), SessionChange::Disconnect); // WTS_CONSOLE_DISCONNECT
    EXPECT_EQ(decodeSessionChange(0x4U), SessionChange::Disconnect); // WTS_REMOTE_DISCONNECT
}

TEST(WindowVisibilityTest, OtherSessionChangesAreOther)
{
    EXPECT_EQ(decodeSessionChange(0x5U), SessionChange::Other); // WTS_SESSION_LOGON
    EXPECT_EQ(decodeSessionChange(0x6U), SessionChange::Other); // WTS_SESSION_LOGOFF
    EXPECT_EQ(decodeSessionChange(0x9U), SessionChange::Other); // WTS_SESSION_REMOTE_CONTROL
    EXPECT_EQ(decodeSessionChange(0xAU), SessionChange::Other); // WTS_SESSION_CREATE
    EXPECT_EQ(decodeSessionChange(0xBU), SessionChange::Other); // WTS_SESSION_TERMINATE
    EXPECT_EQ(decodeSessionChange(0U), SessionChange::Other);
}

// ---- applySessionChange ----

TEST(WindowVisibilityTest, ALockHidesTheWindowUntilTheUnlock)
{
    const Signals locked = applySessionChange(Signals{}, SessionChange::Lock);
    EXPECT_TRUE(locked.sessionLocked);
    EXPECT_TRUE(isHidden(false, false, locked));
    const Signals unlocked = applySessionChange(locked, SessionChange::Unlock);
    EXPECT_EQ(unlocked, Signals{});
    EXPECT_FALSE(isHidden(false, false, unlocked));
}

TEST(WindowVisibilityTest, ADisconnectHidesTheWindowUntilTheNextConnect)
{
    const Signals disconnected = applySessionChange(Signals{}, SessionChange::Disconnect);
    EXPECT_TRUE(disconnected.sessionDisconnected);
    EXPECT_TRUE(isHidden(false, false, disconnected));
    EXPECT_EQ(applySessionChange(disconnected, SessionChange::Connect), Signals{});
}

TEST(WindowVisibilityTest, AReconnectToALockedSessionStaysHiddenUntilTheUnlock)
{
    // A remote client reconnects to the session at its lock screen: still nothing to see.
    Signals signals = applySessionChange(Signals{}, SessionChange::Lock);
    signals = applySessionChange(signals, SessionChange::Disconnect);
    signals = applySessionChange(signals, SessionChange::Connect);
    EXPECT_TRUE(signals.sessionLocked);
    EXPECT_FALSE(signals.sessionDisconnected);
    EXPECT_TRUE(isHidden(false, false, signals));
    EXPECT_EQ(applySessionChange(signals, SessionChange::Unlock), Signals{});
}

TEST(WindowVisibilityTest, AnUnlockEndsADisconnectWhoseConnectWasMissed)
{
    // No stale hidden: someone unlocking the session is using it.
    const Signals signals{.sessionLocked = true, .sessionDisconnected = true};
    EXPECT_EQ(applySessionChange(signals, SessionChange::Unlock), Signals{});
}

TEST(WindowVisibilityTest, OtherSessionChangesAndUnrelatedSignalsAreKept)
{
    const Signals signals{.cloaked = true, .sessionLocked = true, .displayOff = true};
    EXPECT_EQ(applySessionChange(signals, SessionChange::Other), signals);
    // A session change leaves the cloak and the display alone.
    const Signals unlocked = applySessionChange(signals, SessionChange::Unlock);
    EXPECT_TRUE(unlocked.cloaked);
    EXPECT_TRUE(unlocked.displayOff);
    EXPECT_FALSE(unlocked.sessionLocked);
}

// ---- decodeDisplayState ----

TEST(WindowVisibilityTest, DisplayStatesDecodeFromTheirWin32Values)
{
    EXPECT_EQ(decodeDisplayState(dwordBytes(0U)), DisplayState::Off);    // PowerMonitorOff
    EXPECT_EQ(decodeDisplayState(dwordBytes(1U)), DisplayState::On);     // PowerMonitorOn
    EXPECT_EQ(decodeDisplayState(dwordBytes(2U)), DisplayState::Dimmed); // PowerMonitorDim
}

TEST(WindowVisibilityTest, AnUndocumentedDisplayStateIsUnknown)
{
    EXPECT_EQ(decodeDisplayState(dwordBytes(3U)), DisplayState::Unknown);
    EXPECT_EQ(decodeDisplayState(dwordBytes(0xFFFF'FFFFU)), DisplayState::Unknown);
}

TEST(WindowVisibilityTest, DataTooShortForTheStateIsUnknown)
{
    EXPECT_EQ(decodeDisplayState({}), DisplayState::Unknown);
    const auto bytes = dwordBytes(0U);
    EXPECT_EQ(decodeDisplayState(std::span(bytes).first(3)), DisplayState::Unknown);
}

TEST(WindowVisibilityTest, DataLongerThanTheStateReadsItsFirstDword)
{
    std::array<std::byte, 8> bytes{};
    const auto on = dwordBytes(1U);
    std::memcpy(bytes.data(), on.data(), on.size());
    bytes[4] = std::byte{0xFF};
    EXPECT_EQ(decodeDisplayState(bytes), DisplayState::On);
}

// ---- applyDisplayState ----

TEST(WindowVisibilityTest, ADisplayThatTurnsOffHidesTheWindowUntilItIsOn)
{
    const Signals off = applyDisplayState(Signals{}, DisplayState::Off);
    EXPECT_TRUE(off.displayOff);
    EXPECT_TRUE(isHidden(false, false, off));
    EXPECT_EQ(applyDisplayState(off, DisplayState::On), Signals{});
}

TEST(WindowVisibilityTest, ADimmedDisplayStillShowsTheWindow)
{
    EXPECT_EQ(applyDisplayState(Signals{}, DisplayState::Dimmed), Signals{});
    EXPECT_EQ(applyDisplayState(Signals{.displayOff = true}, DisplayState::Dimmed), Signals{});
}

TEST(WindowVisibilityTest, AnUnknownDisplayStateChangesNothing)
{
    const Signals off{.sessionLocked = true, .displayOff = true};
    EXPECT_EQ(applyDisplayState(off, DisplayState::Unknown), off);
    EXPECT_EQ(applyDisplayState(Signals{}, DisplayState::Unknown), Signals{});
}

// ---- cloakRecheckDue ----

TEST(WindowVisibilityTest, AShownWindowIsRecheckedOncePerInterval)
{
    EXPECT_FALSE(cloakRecheckDue(false, 0ms, WINDOW_CLOAK_RECHECK_INTERVAL));
    EXPECT_FALSE(cloakRecheckDue(false, WINDOW_CLOAK_RECHECK_INTERVAL - 1ms, WINDOW_CLOAK_RECHECK_INTERVAL));
    EXPECT_TRUE(cloakRecheckDue(false, WINDOW_CLOAK_RECHECK_INTERVAL, WINDOW_CLOAK_RECHECK_INTERVAL));
    EXPECT_TRUE(cloakRecheckDue(false, 10s, WINDOW_CLOAK_RECHECK_INTERVAL));
}

TEST(WindowVisibilityTest, ACloakedWindowIsRecheckedEveryTime)
{
    // Back on the window's desktop, the normal rate returns on the next frame, not an interval later.
    EXPECT_TRUE(cloakRecheckDue(true, 0ms, WINDOW_CLOAK_RECHECK_INTERVAL));
    EXPECT_TRUE(cloakRecheckDue(true, 1ms, WINDOW_CLOAK_RECHECK_INTERVAL));
}

TEST(WindowVisibilityTest, TheRecheckIntervalIsSlowButBounded)
{
    // A slow timer: a few DWM calls a second at most, and a cloak without an activation or focus
    // change still found within a couple of seconds.
    EXPECT_GE(WINDOW_CLOAK_RECHECK_INTERVAL, 250ms);
    EXPECT_LE(WINDOW_CLOAK_RECHECK_INTERVAL, 2s);
}

} // namespace
} // namespace Core::WindowVisibility
