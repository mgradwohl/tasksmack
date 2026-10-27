#pragma once

// The custom title bar's drawing (#1547): the icon, the wordmark, the window controls and the app
// buttons, the Linux system menu and the window border. TitleBarLayer keeps everything that talks
// to SDL -- the window calls, its minimum size, hit testing, the icon texture -- and carries out the
// returned action, so the bar can be rendered headless in the tests.

#include "App/TitleBarGeometry.h"
#include "Core/WindowGeometry.h"
#include "UI/IconLoader.h"

#include <cstdint>
#include <string_view>

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

/// The item that holds the window's top-right resize corner out of the Close button's hover and
/// press (#2882); named so the headless test can ask ImGui which item the corner hovers.
inline constexpr const char* TOP_RIGHT_RESIZE_CORNER_ID = "##TopRightResizeCorner";

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

/// Which of the system menu's items can be chosen. The same rule for the Linux menu the view draws
/// and the native Windows one TitleBarLayer shows, which Windows doesn't keep up to date for a
/// client-side maximize (#3161).
struct SystemMenuItems
{
    bool restore = false;
    bool move = false;
    bool size = false;
    bool minimize = false;
    bool maximize = false;
    bool close = false;

    [[nodiscard]] constexpr bool operator==(const SystemMenuItems&) const = default;
};

/// The system menu's items for a window that is @p maximized or not: Restore only when maximized,
/// Maximize only when not, and Move and Size only when not, since a maximized window has neither.
[[nodiscard]] constexpr auto systemMenuItems(bool maximized) noexcept -> SystemMenuItems
{
    return {.restore = maximized, .move = !maximized, .size = !maximized, .minimize = true, .maximize = !maximized, .close = true};
}

/// The action for a command chosen from the native Windows system menu (#3161): the window commands
/// the title-bar buttons have are carried out the same way, client-side maximize included. None for
/// anything else (Move, Size), which goes to the OS as WM_SYSCOMMAND.
[[nodiscard]] constexpr auto actionForSystemCommand(Core::WindowGeometry::SystemCommand command) noexcept -> Action
{
    using Core::WindowGeometry::SystemCommand;
    switch (command)
    {
    case SystemCommand::Restore:
        return Action::Restore;
    case SystemCommand::Maximize:
        return Action::Maximize;
    case SystemCommand::Minimize:
        return Action::Minimize;
    case SystemCommand::Close:
        return Action::Close;
    case SystemCommand::Other:
    case SystemCommand::Move:
    case SystemCommand::Size:
        return Action::None;
    }
    return Action::None;
}

/// The bar's inputs for one frame.
struct Input
{
    float windowWidth = 0.0F;
    float windowHeight = 0.0F;
    float barHeight = 0.0F; ///< TitleBarLayer::height()
    bool maximized = false; ///< Decides Maximize vs Restore, and the border
    /// The resize border's thickness (computeResizeBorderThickness()): the side of the top-right
    /// resize corner, which the bar keeps from the Close button's hover and press (#2882). Not used
    /// on a maximized window, which has no borders; 0 keeps the corner the Close button's.
    float resizeBorderPx = 0.0F;
    const UI::Texture* icon = nullptr;
    bool openSystemMenu = false; ///< Open the Linux system menu this frame (Alt+Space)
    /// The window's title, as the OS shows it: "TaskSmack (Administrator)" in an Administrator session (#2728)
    std::string_view title = "TaskSmack";
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
