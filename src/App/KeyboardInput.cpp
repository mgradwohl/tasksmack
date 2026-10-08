#include "KeyboardInput.h"

#include "App/KeyboardShortcuts.h"
#include "App/Panels/ProcessTableNavigation.h"

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // SetKeyOwner(), ScrollToRect() and the table's inner window: no public accessor
// clang-format on

#include <array>
#include <utility>

namespace App::KeyboardInput
{

namespace
{

using KeyboardShortcuts::FunctionKey;
using ProcessTableNavigation::NavCommand;
using ProcessTableNavigation::NavKey;

constexpr std::array<std::pair<FunctionKey, ImGuiKey>, 5> FUNCTION_KEYS{{
    {FunctionKey::F1, ImGuiKey_F1},
    {FunctionKey::F2, ImGuiKey_F2},
    {FunctionKey::F5, ImGuiKey_F5},
    {FunctionKey::F9, ImGuiKey_F9},
    {FunctionKey::F10, ImGuiKey_F10},
}};

/// A navigation key, its ImGui key, and whether holding it repeats.
struct NavKeyBinding
{
    NavKey key;
    ImGuiKey imguiKey;
    bool repeat;
};

constexpr std::array<NavKeyBinding, 11> NAV_KEYS{{
    {.key = NavKey::Up, .imguiKey = ImGuiKey_UpArrow, .repeat = true},
    {.key = NavKey::Down, .imguiKey = ImGuiKey_DownArrow, .repeat = true},
    {.key = NavKey::PageUp, .imguiKey = ImGuiKey_PageUp, .repeat = true},
    {.key = NavKey::PageDown, .imguiKey = ImGuiKey_PageDown, .repeat = true},
    {.key = NavKey::Home, .imguiKey = ImGuiKey_Home, .repeat = false},
    {.key = NavKey::End, .imguiKey = ImGuiKey_End, .repeat = false},
    {.key = NavKey::Left, .imguiKey = ImGuiKey_LeftArrow, .repeat = true},
    {.key = NavKey::Right, .imguiKey = ImGuiKey_RightArrow, .repeat = true},
    {.key = NavKey::J, .imguiKey = ImGuiKey_J, .repeat = true},
    {.key = NavKey::K, .imguiKey = ImGuiKey_K, .repeat = true},
    {.key = NavKey::G, .imguiKey = ImGuiKey_G, .repeat = false},
}};

[[nodiscard]] constexpr bool isHorizontal(NavKey key) noexcept
{
    return key == NavKey::Left || key == NavKey::Right;
}

/// The keys ImGui's keyboard navigation moves and scrolls with. The letters are not among them.
[[nodiscard]] constexpr bool isImGuiNavKey(NavKey key) noexcept
{
    return key != NavKey::J && key != NavKey::K && key != NavKey::G;
}

[[nodiscard]] bool anyPopupOpen()
{
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

} // namespace

KeyboardShortcuts::InputState currentInputState()
{
    const ImGuiIO& io = ImGui::GetIO();
    return KeyboardShortcuts::InputState{
        .textInputActive = io.WantTextInput,
        .popupOpen = anyPopupOpen(),
        .ctrl = io.KeyCtrl,
        .shift = io.KeyShift,
        .alt = io.KeyAlt,
        .super = io.KeySuper,
    };
}

KeyboardShortcuts::ShortcutAction pollFunctionKeys()
{
    const KeyboardShortcuts::InputState state = currentInputState();
    for (const auto& [key, imguiKey] : FUNCTION_KEYS)
    {
        if (ImGui::IsKeyPressed(imguiKey, /*repeat=*/false))
        {
            return KeyboardShortcuts::actionFor(key, state);
        }
    }
    return KeyboardShortcuts::ShortcutAction::None;
}

bool tableNavigationArmed()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || anyPopupOpen())
    {
        return false;
    }
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
}

void claimNavigationKeys(unsigned int ownerId, bool treeView)
{
    for (const NavKeyBinding& binding : NAV_KEYS)
    {
        if (isImGuiNavKey(binding.key) && (treeView || !isHorizontal(binding.key)))
        {
            ImGui::SetKeyOwner(binding.imguiKey, ownerId);
        }
    }
}

NavCommand pollNavigationCommand(bool treeView)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ProcessTableNavigation::NavModifiers mods{.ctrl = io.KeyCtrl, .shift = io.KeyShift, .alt = io.KeyAlt, .super = io.KeySuper};
    for (const NavKeyBinding& binding : NAV_KEYS)
    {
        if (!treeView && isHorizontal(binding.key))
        {
            continue;
        }
        if (ImGui::IsKeyPressed(binding.imguiKey, binding.repeat))
        {
            return ProcessTableNavigation::commandFor(binding.key, mods);
        }
    }
    return NavCommand::None;
}

float tableScrollViewHeight()
{
    const ImGuiTable* table = ImGui::GetCurrentTable();
    if (table == nullptr || table->InnerWindow == nullptr)
    {
        return 0.0F;
    }
    // DecoInnerSizeY1 is the frozen header's height, which ImGui itself leaves out when scrolling.
    const ImGuiWindow* window = table->InnerWindow;
    return window->InnerRect.GetHeight() - window->DecoInnerSizeY1;
}

void scrollLastItemIntoView()
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    // Only the row's vertical extent: given at an x already in view, ScrollToRect() leaves the
    // horizontal scroll alone (a row spans every column, wider than the view, which would scroll left).
    const float visibleX = window->InnerClipRect.Min.x;
    ImGui::ScrollToRect(window, ImRect(visibleX, itemMin.y, visibleX, itemMax.y), ImGuiScrollFlags_KeepVisibleEdgeY);
}

} // namespace App::KeyboardInput
