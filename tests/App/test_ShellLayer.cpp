/// @file test_ShellLayer.cpp
/// @brief Tests for App::ShellLayer type properties
///
/// ShellLayer is deeply coupled to ImGui (onUpdate calls ImGui::GetIO() for keyboard
/// shortcuts, onRender uses the full ImGui rendering API) and owns live panel objects
/// that start background sampling threads on attach. Linking ShellLayer.cpp into the
/// test binary requires imgui_lib, implot_lib, and all panel .cpp files — a scope that
/// belongs in a future "integration test with ImGui context" effort.
///
/// This file tests the type-level properties that can be verified from the header alone:
/// - Copy/move semantics (deleted per Rule of 5)
/// - Default constructibility
///
/// See also: tests/App/test_ActiveTab.cpp for the ActiveTab enum tests.

#include "App/ShellLayer.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace
{

// =============================================================================
// Type Safety Tests (header-only, no instantiation required)
// =============================================================================

TEST(ShellLayerTest, ShellLayerIsNotCopyConstructible)
{
    static_assert(!std::is_copy_constructible_v<App::ShellLayer>,
                  "ShellLayer should not be copy-constructible (owns non-copyable panels and samplers)");
    SUCCEED();
}

TEST(ShellLayerTest, ShellLayerIsNotCopyAssignable)
{
    static_assert(!std::is_copy_assignable_v<App::ShellLayer>, "ShellLayer should not be copy-assignable");
    SUCCEED();
}

TEST(ShellLayerTest, ShellLayerIsNotMoveConstructible)
{
    static_assert(!std::is_move_constructible_v<App::ShellLayer>, "ShellLayer should not be move-constructible (panels are non-movable)");
    SUCCEED();
}

TEST(ShellLayerTest, ShellLayerIsNotMoveAssignable)
{
    static_assert(!std::is_move_assignable_v<App::ShellLayer>, "ShellLayer should not be move-assignable");
    SUCCEED();
}

TEST(ShellLayerTest, ShellLayerIsDefaultConstructible)
{
    // ShellLayer() is declared and not deleted; it should be constructible in contexts
    // where its panels can be default-constructed (i.e., with the full app libs linked).
    static_assert(std::is_default_constructible_v<App::ShellLayer>, "ShellLayer should be default-constructible");
    SUCCEED();
}

} // namespace
