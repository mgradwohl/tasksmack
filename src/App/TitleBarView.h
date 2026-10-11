#pragma once

// The custom title bar's drawing (#1547): the icon, the wordmark, the window controls and the app
// buttons, the Linux system menu and the window border. TitleBarLayer keeps everything that talks
// to SDL -- the window calls, its minimum size, hit testing, the icon texture -- and carries out the
// returned action, so the bar can be rendered headless in the tests.

#include "App/TitleBarGeometry.h"
#include "UI/IconLoader.h"

#include <cstdint>

namespace App
{

// How far the icon is inset from the bar's full height, as a fraction of that height.
inline constexpr float TITLE_BAR_ICON_INSET_RATIO = 0.06F;

// Window control button width as a multiple of the bar height. Keeps the controls' aspect stable
// instead of pinning them to a pixel width that is too small on a HiDPI display.
inline constexpr float TITLE_BAR_BUTTON_ASPECT = 1.15F;

// Left margin before the icon, and the gap between icon and title text, as fractions of the bar
// height. Reproduce the previous 8px and 12px at the default density.
inline constexpr float TITLE_BAR_EDGE_MARGIN_RATIO = 0.20F;
inline constexpr float TITLE_BAR_TITLE_GAP_RATIO = 0.29F;

// The bar window's padding, as fractions of the bar height: exactly the former fixed 8 x 4 px on
// the 32px (24pt) bar at 96 DPI (#1200).
inline constexpr float TITLE_BAR_PADDING_X_RATIO = 0.25F;
inline constexpr float TITLE_BAR_PADDING_Y_RATIO = 0.125F;

// Gap separating the window controls from the app buttons, as a fraction of the bar height.
inline constexpr float TITLE_BAR_SEPARATOR_GAP_RATIO = 0.39F;

namespace TitleBarView
{

/// The Linux system menu's popup ID (the icon's menu, also opened by Alt+Space).
inline constexpr const char* SYSTEM_MENU_ID = "##SystemMenu";

/// What the user asked for this frame, for TitleBarLayer to carry out.
enum class Action : std::uint8_t
{
    None,
    IconClicked, ///< The app icon: Windows shows the native system menu (Linux opens its own here)
    Close,
    Maximize,
    Restore,
    Minimize,
    Settings,
    Help,
    About,
};

/// The bar's inputs for one frame.
struct Input
{
    float windowWidth = 0.0F;
    float windowHeight = 0.0F;
    float barHeight = 0.0F; ///< TitleBarLayer::height()
    bool maximized = false; ///< Decides Maximize vs Restore, and the border
    const UI::Texture* icon = nullptr;
    bool openSystemMenu = false; ///< Open the Linux system menu this frame (Alt+Space)
};

/// What the frame drew, for the layer's hit testing and minimum size.
struct Output
{
    Action action = Action::None;
    TitleBarButtonLayout layout{}; ///< Where the buttons are (kept out of the drag area)
    ButtonBounds iconBounds{};     ///< Where the icon is, when it was drawn
    bool iconDrawn = false;
    float contentWidth = 0.0F; ///< The width the bar needs: icon, wordmark, buttons and gaps
};

/// The icon's drawn size for a bar of @p barHeight, so the layer can load the matching texture first.
[[nodiscard]] float iconSize(float barHeight) noexcept;

/// Draw the title bar for this frame and report what was asked for.
[[nodiscard]] Output render(const Input& input);

} // namespace TitleBarView
} // namespace App
