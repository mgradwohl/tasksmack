/// @file test_ProcessDisplayFreeze.cpp
/// @brief Tests for App::ProcessDisplayFreeze: hold Ctrl to freeze the Processes pane (#928)

#include "App/Panels/ProcessDisplayFreeze.h"

#include <gtest/gtest.h>

namespace
{

using App::ProcessDisplayFreeze::ENGAGE_DELAY_SECONDS;
using App::ProcessDisplayFreeze::Inputs;
using App::ProcessDisplayFreeze::isFreezeGesture;
using App::ProcessDisplayFreeze::Tracker;

// Comfortably past the delay from a non-zero start, where start + ENGAGE_DELAY_SECONDS - start need
// not round back to exactly ENGAGE_DELAY_SECONDS.
constexpr double PAST_DELAY = ENGAGE_DELAY_SECONDS + 0.001;

/// Ctrl alone, held in a focused app with the pointer over the pane: the gesture.
[[nodiscard]] constexpr Inputs ctrlOverPanel() noexcept
{
    return Inputs{.appFocused = true, .ctrlHeld = true, .panelHovered = true};
}

[[nodiscard]] constexpr Inputs released() noexcept
{
    return Inputs{.appFocused = true, .panelHovered = true};
}

/// Holds `in` from t=0 past the engage delay and returns whether the tracker froze.
[[nodiscard]] bool frozenAfterHold(Tracker& tracker, const Inputs& in)
{
    static_cast<void>(tracker.update(in, 0.0));
    return tracker.update(in, PAST_DELAY);
}

// --- isFreezeGesture ---------------------------------------------------------------------------

TEST(ProcessDisplayFreezeTest, CtrlHeldOverHoveredPanelIsTheGesture)
{
    EXPECT_TRUE(isFreezeGesture(ctrlOverPanel()));
}

TEST(ProcessDisplayFreezeTest, CtrlHeldWithPanelFocusedIsTheGesture)
{
    Inputs in = ctrlOverPanel();
    in.panelHovered = false;
    in.panelFocused = true;
    EXPECT_TRUE(isFreezeGesture(in));
}

TEST(ProcessDisplayFreezeTest, CtrlHeldWithPanelNeitherHoveredNorFocusedIsNot)
{
    Inputs in = ctrlOverPanel();
    in.panelHovered = false;
    in.panelFocused = false;
    EXPECT_FALSE(isFreezeGesture(in));
}

TEST(ProcessDisplayFreezeTest, NoCtrlIsNotTheGesture)
{
    EXPECT_FALSE(isFreezeGesture(released()));
}

TEST(ProcessDisplayFreezeTest, ActiveTextInputIsNotTheGesture)
{
    // Ctrl in the filter box is text editing (Ctrl+A, Ctrl+Left), not a freeze.
    Inputs in = ctrlOverPanel();
    in.textInputActive = true;
    EXPECT_FALSE(isFreezeGesture(in));
}

TEST(ProcessDisplayFreezeTest, UnfocusedAppWindowIsNotTheGesture)
{
    Inputs in = ctrlOverPanel();
    in.appFocused = false;
    EXPECT_FALSE(isFreezeGesture(in));
}

TEST(ProcessDisplayFreezeTest, CtrlWithAnotherKeyIsAShortcutNotTheGesture)
{
    Inputs in = ctrlOverPanel();
    in.otherKeyHeld = true; // Ctrl+C, Ctrl+F, Ctrl+=
    EXPECT_FALSE(isFreezeGesture(in));
}

TEST(ProcessDisplayFreezeTest, CtrlWithAnotherModifierIsAShortcutNotTheGesture)
{
    Inputs in = ctrlOverPanel();
    in.otherModifierHeld = true; // Ctrl+Shift+M
    EXPECT_FALSE(isFreezeGesture(in));
}

TEST(ProcessDisplayFreezeTest, GestureIsUsableAtCompileTime)
{
    static_assert(isFreezeGesture(ctrlOverPanel()));
    static_assert(!isFreezeGesture(released()));
}

// --- Tracker -----------------------------------------------------------------------------------

TEST(ProcessDisplayFreezeTest, StartsUnfrozen)
{
    const Tracker tracker;
    EXPECT_FALSE(tracker.frozen());
}

TEST(ProcessDisplayFreezeTest, FreezesOnlyAfterTheEngageDelay)
{
    Tracker tracker;
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 0.0));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), ENGAGE_DELAY_SECONDS / 2.0));
    EXPECT_TRUE(tracker.update(ctrlOverPanel(), ENGAGE_DELAY_SECONDS)); // Exactly the delay engages
    EXPECT_TRUE(tracker.frozen());
}

TEST(ProcessDisplayFreezeTest, ReleasingCtrlUnfreezesAtOnce)
{
    Tracker tracker;
    ASSERT_TRUE(frozenAfterHold(tracker, ctrlOverPanel()));
    EXPECT_FALSE(tracker.update(released(), 1.0));
    EXPECT_FALSE(tracker.frozen());
}

TEST(ProcessDisplayFreezeTest, HoldingAgainRestartsTheDelay)
{
    Tracker tracker;
    ASSERT_TRUE(frozenAfterHold(tracker, ctrlOverPanel()));
    static_cast<void>(tracker.update(released(), 1.0));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 2.0));
    EXPECT_TRUE(tracker.update(ctrlOverPanel(), 2.0 + PAST_DELAY));
}

TEST(ProcessDisplayFreezeTest, NeverFreezesWithoutHoverOrFocus)
{
    Tracker tracker;
    Inputs in = ctrlOverPanel();
    in.panelHovered = false;
    EXPECT_FALSE(frozenAfterHold(tracker, in));
}

TEST(ProcessDisplayFreezeTest, NeverFreezesWhileTextInputIsActive)
{
    Tracker tracker;
    Inputs in = ctrlOverPanel();
    in.textInputActive = true;
    EXPECT_FALSE(frozenAfterHold(tracker, in));
}

TEST(ProcessDisplayFreezeTest, NeverFreezesWhileTheAppIsUnfocused)
{
    Tracker tracker;
    Inputs in = ctrlOverPanel();
    in.appFocused = false;
    EXPECT_FALSE(frozenAfterHold(tracker, in));
}

TEST(ProcessDisplayFreezeTest, LosingAppFocusUnfreezes)
{
    Tracker tracker;
    ASSERT_TRUE(frozenAfterHold(tracker, ctrlOverPanel()));
    Inputs in = ctrlOverPanel();
    in.appFocused = false;
    EXPECT_FALSE(tracker.update(in, 1.0));
}

TEST(ProcessDisplayFreezeTest, PointerLeavingThePanelUnfreezes)
{
    Tracker tracker;
    ASSERT_TRUE(frozenAfterHold(tracker, ctrlOverPanel()));
    Inputs in = ctrlOverPanel();
    in.panelHovered = false;
    EXPECT_FALSE(tracker.update(in, 1.0));
}

TEST(ProcessDisplayFreezeTest, ChordKeyPressedBeforeTheDelayNeverFreezes)
{
    // Ctrl, then C 50 ms later: the Ctrl+C shortcut must not flash the freeze.
    Tracker tracker;
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 0.0));
    Inputs chord = ctrlOverPanel();
    chord.otherKeyHeld = true;
    EXPECT_FALSE(tracker.update(chord, 0.05));
    EXPECT_FALSE(tracker.update(chord, 1.0));
}

TEST(ProcessDisplayFreezeTest, ChordStaysUnfrozenUntilCtrlIsReleased)
{
    // Ctrl held across repeated Ctrl+= presses: the gaps between presses are Ctrl alone, but they
    // belong to the shortcut, so the pane must not freeze in them.
    Tracker tracker;
    Inputs chord = ctrlOverPanel();
    chord.otherKeyHeld = true;
    EXPECT_FALSE(tracker.update(chord, 0.0));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 0.1));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 0.1 + (ENGAGE_DELAY_SECONDS * 4.0)));

    // Once Ctrl is released, the next hold is a fresh gesture.
    EXPECT_FALSE(tracker.update(released(), 2.0));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 3.0));
    EXPECT_TRUE(tracker.update(ctrlOverPanel(), 3.0 + PAST_DELAY));
}

TEST(ProcessDisplayFreezeTest, ModifierChordStaysUnfrozenUntilCtrlIsReleased)
{
    // Ctrl+Shift+M, with Shift let go before Ctrl.
    Tracker tracker;
    Inputs chord = ctrlOverPanel();
    chord.otherModifierHeld = true;
    EXPECT_FALSE(tracker.update(chord, 0.0));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 0.5));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 1.5));
}

TEST(ProcessDisplayFreezeTest, ShortcutWhileFrozenUnfreezes)
{
    Tracker tracker;
    ASSERT_TRUE(frozenAfterHold(tracker, ctrlOverPanel()));
    Inputs chord = ctrlOverPanel();
    chord.otherKeyHeld = true;
    EXPECT_FALSE(tracker.update(chord, 1.0));
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 2.0));
}

TEST(ProcessDisplayFreezeTest, ResetUnfreezesAndForgetsTheGesture)
{
    Tracker tracker;
    ASSERT_TRUE(frozenAfterHold(tracker, ctrlOverPanel()));
    tracker.reset();
    EXPECT_FALSE(tracker.frozen());
    // Still held after the reset: the delay starts over.
    EXPECT_FALSE(tracker.update(ctrlOverPanel(), 5.0));
    EXPECT_TRUE(tracker.update(ctrlOverPanel(), 5.0 + PAST_DELAY));
}

} // namespace
