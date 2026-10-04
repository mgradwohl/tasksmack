#pragma once

#include "Core/WindowConstants.h"

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace App
{

enum class ResizeEdge : std::uint8_t
{
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// The title bar's height is not computed here. It is specified in points in UILayer, converted once
// against the display scale and stored on Theme (see Theme::titleBarHeightPx), for two reasons.
//
// It must be independent of the application's Font Size setting: the title bar is chrome, not
// content, its label is drawn in the fixed Sixtyfour display font, and its icon and window buttons
// are sized from this height, so tying it to the body font made the icon and buttons balloon when
// the user picked a larger font -- which is not how a window frame behaves on either platform.
//
// It must also be answerable outside an ImGui frame, which fixes a latent bug rather than only a
// cosmetic one. SDL calls hitTestCallback() with no frame in flight, and that callback needs this
// height to decide what is a drag region and what is a resize edge. A body-font-derived height
// queried there returned a 6px title bar (ImGui::GetFontSize() is 0 with no font pushed), so drag
// and resize hit-testing was computed against nonsense.
//
// Everything below takes that height as a parameter and touches no ImGui state, so it is valid at
// any time.

/// Size of the square application icon drawn at the left of the title bar.
///
/// Inset from the bar height so the icon never touches the window edge. Clamped at zero because a
/// degenerate bar height would otherwise ask ImGui::Image() for a negative extent.
[[nodiscard]] inline auto computeTitleBarIconSize(const float titleBarHeightPx, const float insetPx) -> float
{
    return std::max(0.0F, titleBarHeightPx - insetPx);
}

/// Width of one window control button (minimize / maximize / close).
///
/// Proportional to the bar height so the controls keep their aspect as the bar scales with display
/// density, instead of staying at a fixed pixel width that is too small on a HiDPI display.
[[nodiscard]] inline auto computeTitleBarButtonWidth(const float titleBarHeightPx, const float aspect) -> float
{
    return std::max(0.0F, titleBarHeightPx * aspect);
}

/// Font size at which one glyph must be drawn for its ink to come out the same size as a reference
/// glyph drawn at `referenceSizePx`.
///
/// Font Awesome's control glyphs do not all fill their em box. Measured from fa-solid-900.ttf, every
/// one of window-minimize, window-maximize, window-restore and circle-question spans the full
/// 1.000 em of ink width, and gear 0.953 em, but fa-xmark spans only 0.625 em. Drawn at one shared
/// font size the close button's X therefore comes out 37% smaller than the icons beside it and reads
/// as a lighter, different control. Rather than hard-code that ratio -- which would silently go
/// stale if the icon font were replaced or restyled -- callers measure both glyphs' ink from the
/// baked font and pass the measurements here.
///
/// Match on ink *width*, not height: the heights disagree between glyphs that are equally wide
/// (window-maximize is 0.875 em tall against window-restore's 1.000 em), and the maximize button
/// swaps between those two with the window state, so a height reference taken from it would resize
/// the close button every time the window is maximized.
///
/// @param referenceSizePx  Font size the reference glyph is drawn at.
/// @param referenceInkPx   Reference glyph's ink width at that size.
/// @param glyphInkPx       This glyph's ink width at that same size.
/// @return Font size for this glyph, or referenceSizePx if either measurement is unusable.
[[nodiscard]] inline auto computeMatchedGlyphSize(const float referenceSizePx, const float referenceInkPx, const float glyphInkPx) -> float
{
    if (referenceSizePx <= 0.0F || referenceInkPx <= 0.0F || glyphInkPx <= 0.0F)
    {
        return std::max(0.0F, referenceSizePx);
    }
    return referenceSizePx * (referenceInkPx / glyphInkPx);
}

/// Width of the strip along each window edge that starts a resize, at a 1.0 display scale.
inline constexpr float RESIZE_BORDER_REFERENCE_PX = 8.0F;

/// Width of the resize strip at the given display scale, in whole pixels.
///
/// The window is borderless, so this strip is the only resize affordance it has. As a fixed 8px it
/// was compared against coordinates in which the title bar is display-scaled, so at 175-200% it was
/// half the physical size it is at 100% (#970). Scaled, it is a constant physical size like the bar.
/// Never thinner than the reference: a scale below 1.0 must not shrink an already small target.
///
/// @param displayScale  UI scale in window units (UI::windowUnitScale); 1.0 at 96 DPI.
[[nodiscard]] inline auto computeResizeBorderThickness(const float displayScale) -> float
{
    const float scale = (std::isfinite(displayScale) && displayScale > 1.0F) ? displayScale : 1.0F;
    return std::round(RESIZE_BORDER_REFERENCE_PX * scale);
}

/// Width the title bar's own content needs: the icon, the wordmark, and the five buttons, with the
/// gaps the bar draws between them.
///
/// Every input is a size the bar has already computed for drawing, so the minimum window width
/// derived from this cannot drift from what is actually on screen.
///
/// @param edgeMarginPx    Left margin before the icon.
/// @param iconSizePx      Width of the application icon.
/// @param titleGapPx      Gap between the icon and the wordmark; reused between wordmark and buttons.
/// @param wordmarkWidthPx Measured width of the "TaskSmack" wordmark; 0 before it has been drawn.
/// @param buttonWidthPx   Width of one title-bar button.
/// @param separatorGapPx  Gap between the two app buttons and the three window buttons.
[[nodiscard]] inline auto computeTitleBarContentWidth(const float edgeMarginPx,
                                                      const float iconSizePx,
                                                      const float titleGapPx,
                                                      const float wordmarkWidthPx,
                                                      const float buttonWidthPx,
                                                      const float separatorGapPx) -> float
{
    constexpr float BUTTON_COUNT = 5.0F; // help, settings, minimize, maximize, close
    const auto atLeastZero = [](const float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };

    const float leading = atLeastZero(edgeMarginPx) + atLeastZero(iconSizePx) + atLeastZero(titleGapPx) + atLeastZero(wordmarkWidthPx);
    const float trailing = (atLeastZero(buttonWidthPx) * BUTTON_COUNT) + atLeastZero(separatorGapPx);
    return leading + atLeastZero(titleGapPx) + trailing;
}

/// Smallest size the window may be resized to.
struct WindowMinimumSize
{
    int width = Core::WINDOW_MIN_DIMENSION;
    int height = Core::WINDOW_MIN_DIMENSION;

    friend constexpr auto operator==(const WindowMinimumSize&, const WindowMinimumSize&) -> bool = default;
};

/// Narrowest a plot beside its NowBar column may get, in ems: below this the Overview's charts are
/// a sliver next to their bars (#1207).
inline constexpr float MIN_PLOT_WIDTH_EM = 15.0F;

/// Widest content the window has to show without clipping or overlap, in pixels (#1207): the
/// Processes toolbar row (filter, clear button, process count and the tree-view toggle on one
/// line), or a NowBar column plus MIN_PLOT_WIDTH_EM of plot, whichever is wider, plus the
/// horizontal chrome around the content (gutters and padding).
///
/// @param processesToolbarWidthPx  Width the Processes toolbar row needs; 0 if not known.
/// @param nowBarColumnWidthPx      Width of the Overview's NowBar column with the cell padding that
///                                 separates it from the plot; 0 if not known.
/// @param emPx                     One em, i.e. ImGui::GetFontSize().
/// @param horizontalChromePx       Gutters and padding left and right of the content.
[[nodiscard]] inline auto computeContentMinimumWidth(const float processesToolbarWidthPx,
                                                     const float nowBarColumnWidthPx,
                                                     const float emPx,
                                                     const float horizontalChromePx) -> float
{
    const auto atLeastZero = [](const float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float chartRow = atLeastZero(nowBarColumnWidthPx) + (MIN_PLOT_WIDTH_EM * atLeastZero(emPx));
    return std::max(atLeastZero(processesToolbarWidthPx), chartRow) + atLeastZero(horizontalChromePx);
}

/// Smallest size the window may take: the base minimum at the display's scale, and never narrower
/// than the title bar's own content or the panels' content.
///
/// The base minimum was 200 units at every scale, but the title bar is display-scaled: its five
/// buttons alone need about 197px at 100% and about 393px at 200%. So the window could be dragged
/// narrower than its own title bar, and the buttons were drawn over the wordmark (#970). The panels
/// are font-scaled too: at 200 units the Processes toolbar collided and the Overview's plots were
/// about 50px wide (#1207).
///
/// @param displayScale             UI scale in window units (UI::windowUnitScale); 1.0 at 96 DPI.
/// @param titleBarContentWidthPx   From computeTitleBarContentWidth(); 0 if not yet known.
/// @param contentMinimumWidthPx    From computeContentMinimumWidth(); 0 if not yet known.
[[nodiscard]] inline auto computeMinimumWindowSize(const float displayScale,
                                                   const float titleBarContentWidthPx,
                                                   const float contentMinimumWidthPx = 0.0F) -> WindowMinimumSize
{
    const float scale = (std::isfinite(displayScale) && displayScale > 1.0F) ? displayScale : 1.0F;
    const auto atLeastZero = [](const float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float content = std::max(atLeastZero(titleBarContentWidthPx), atLeastZero(contentMinimumWidthPx));

    const float base = std::round(static_cast<float>(Core::WINDOW_MIN_DIMENSION) * scale);
    const auto maxDimension = static_cast<float>(Core::WINDOW_MAX_DIMENSION);
    // Narrowing: both operands are clamped to [WINDOW_MIN_DIMENSION, WINDOW_MAX_DIMENSION] first.
    return {.width = static_cast<int>(std::min(std::max(base, std::ceil(content)), maxDimension)),
            .height = static_cast<int>(std::min(base, maxDimension))};
}

/// The minimum window size held inside the usable bounds of the display the window is on (#1207).
///
/// The content-derived minimum grows with the font size and the display scale, so on a small
/// display at a large font it can be wider than the screen's work area. Uncapped, the window could
/// not fit on-screen and maximize could not satisfy its own minimum. Capped, the panels lay out
/// narrower than their content wants instead (see computeContentMinimumWidth()).
///
/// @param minimum       From computeMinimumWindowSize().
/// @param usableWidth   Width of the display's usable bounds (SDL_GetDisplayUsableBounds), in the
///                      same window coordinates as @p minimum; 0 or less when unknown, which leaves
///                      the width uncapped.
/// @param usableHeight  Likewise for the height.
/// @return @p minimum, each dimension capped to the usable one but never below
///         Core::WINDOW_MIN_DIMENSION, the floor computeResizeGeometry() holds a drag to.
[[nodiscard]] constexpr auto capMinimumToUsable(const WindowMinimumSize minimum, const int usableWidth, const int usableHeight)
    -> WindowMinimumSize
{
    const auto cap = [](const int wanted, const int usable)
    {
        if (usable <= 0 || wanted <= usable)
        {
            return wanted;
        }
        return std::max(usable, Core::WINDOW_MIN_DIMENSION);
    };
    return {.width = cap(minimum.width, usableWidth), .height = cap(minimum.height, usableHeight)};
}

/// Whether the status bar's right-aligned FPS readout fits beside what is on its left (#1207).
/// At narrow widths it was drawn over "Ready" and the status-bar buttons; it is left out instead.
///
/// @param leftContentEndX  Where the status bar's left-hand content ends (window-local X).
/// @param readoutStartX    Where the right-aligned readout would start (window-local X).
/// @param spacingPx        Gap to keep between them (ImGuiStyle::ItemSpacing.x).
[[nodiscard]] inline auto computeStatusBarReadoutFits(const float leftContentEndX, const float readoutStartX, const float spacingPx) -> bool
{
    const float spacing = (std::isfinite(spacingPx) && spacingPx > 0.0F) ? spacingPx : 0.0F;
    return std::isfinite(leftContentEndX) && std::isfinite(readoutStartX) && readoutStartX >= leftContentEndX + spacing;
}

/// Screen-space rectangle for a title-bar button's hit area (icon, help, settings,
/// minimize, maximize, close). A non-positive width (maxX <= minX) is treated as "not set"
/// by computeIsPointInBounds below, so a default-constructed ButtonBounds never matches.
struct ButtonBounds
{
    float minX = 0.0F;
    float maxX = 0.0F;
    float minY = 0.0F;
    float maxY = 0.0F;
};

/// Pure point-in-rect test for a single button's bounds. No member state - directly
/// unit-testable, like the other geometry helpers in this header.
[[nodiscard]] inline auto computeIsPointInBounds(const float x, const float y, const ButtonBounds& bounds) -> bool
{
    if (bounds.maxX <= bounds.minX)
    {
        return false;
    }
    return x >= bounds.minX && x <= bounds.maxX && y >= bounds.minY && y <= bounds.maxY;
}

/// Pure decision for TitleBarLayer::detectResizeEdge(): which window edge/corner (x, y) is
/// near, given the window size and resize-border thickness. isMaximized short-circuits to
/// None since a maximized window has no resize borders. Directly unit-testable, like the
/// hit-test helpers below.
[[nodiscard]] inline auto computeDetectResizeEdge(
    const float x, const float y, const int windowWidth, const int windowHeight, const bool isMaximized, const float resizeBorderThickness)
    -> ResizeEdge
{
    if (isMaximized)
    {
        return ResizeEdge::None;
    }

    const bool nearLeft = x < resizeBorderThickness;
    const bool nearRight = x >= (static_cast<float>(windowWidth) - resizeBorderThickness);
    const bool nearTop = y < resizeBorderThickness;
    const bool nearBottom = y >= (static_cast<float>(windowHeight) - resizeBorderThickness);

    if (nearTop && nearLeft)
    {
        return ResizeEdge::TopLeft;
    }
    if (nearTop && nearRight)
    {
        return ResizeEdge::TopRight;
    }
    if (nearBottom && nearLeft)
    {
        return ResizeEdge::BottomLeft;
    }
    if (nearBottom && nearRight)
    {
        return ResizeEdge::BottomRight;
    }
    if (nearLeft)
    {
        return ResizeEdge::Left;
    }
    if (nearRight)
    {
        return ResizeEdge::Right;
    }
    if (nearTop)
    {
        return ResizeEdge::Top;
    }
    if (nearBottom)
    {
        return ResizeEdge::Bottom;
    }
    return ResizeEdge::None;
}

/// Geometry returned by computeResizeGeometry — clamped new window rect.
struct WindowRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// Pure geometry function: given the active resize edge and the mouse delta
/// from drag start, compute the clamped window rect. No member state — pure
/// inputs/outputs.
///
/// minWidth/minHeight default to the base minimum; the title bar passes the display-scaled minimum
/// from computeMinimumWindowSize() so a drag stops before the bar's own content is squeezed (#970).
[[nodiscard]] inline auto computeResizeGeometry(const ResizeEdge edge,
                                                const int startX,
                                                const int startY,
                                                const int startWidth,
                                                const int startHeight,
                                                const int dx,
                                                const int dy,
                                                const int minWidth = Core::WINDOW_MIN_DIMENSION,
                                                const int minHeight = Core::WINDOW_MIN_DIMENSION) -> WindowRect
{
    int newX = startX;
    int newY = startY;
    int newWidth = startWidth;
    int newHeight = startHeight;

    switch (edge)
    {
    case ResizeEdge::Left:
        newX = startX + dx;
        newWidth = startWidth - dx;
        break;
    case ResizeEdge::Right:
        newWidth = startWidth + dx;
        break;
    case ResizeEdge::Top:
        newY = startY + dy;
        newHeight = startHeight - dy;
        break;
    case ResizeEdge::Bottom:
        newHeight = startHeight + dy;
        break;
    case ResizeEdge::TopLeft:
        newX = startX + dx;
        newWidth = startWidth - dx;
        newY = startY + dy;
        newHeight = startHeight - dy;
        break;
    case ResizeEdge::TopRight:
        newWidth = startWidth + dx;
        newY = startY + dy;
        newHeight = startHeight - dy;
        break;
    case ResizeEdge::BottomLeft:
        newX = startX + dx;
        newWidth = startWidth - dx;
        newHeight = startHeight + dy;
        break;
    case ResizeEdge::BottomRight:
        newWidth = startWidth + dx;
        newHeight = startHeight + dy;
        break;
    case ResizeEdge::None:
        break;
    }

    // Clamp to min/max and pin the stationary edge when the resize origin is on
    // the left or top so the opposite edge stays anchored.
    constexpr int MAX_W = Core::WINDOW_MAX_DIMENSION;
    constexpr int MAX_H = Core::WINDOW_MAX_DIMENSION;
    // A caller-supplied minimum is held inside the absolute bounds, so it can neither undercut the
    // base minimum nor exceed the maximum and invert the clamp.
    const int lowestWidth = std::clamp(minWidth, Core::WINDOW_MIN_DIMENSION, MAX_W);
    const int lowestHeight = std::clamp(minHeight, Core::WINDOW_MIN_DIMENSION, MAX_H);

    if (newWidth < lowestWidth)
    {
        if (edge == ResizeEdge::Left || edge == ResizeEdge::TopLeft || edge == ResizeEdge::BottomLeft)
        {
            newX = startX + (startWidth - lowestWidth);
        }
        newWidth = lowestWidth;
    }
    if (newWidth > MAX_W)
    {
        if (edge == ResizeEdge::Left || edge == ResizeEdge::TopLeft || edge == ResizeEdge::BottomLeft)
        {
            newX = startX + (startWidth - MAX_W);
        }
        newWidth = MAX_W;
    }
    if (newHeight < lowestHeight)
    {
        if (edge == ResizeEdge::Top || edge == ResizeEdge::TopLeft || edge == ResizeEdge::TopRight)
        {
            newY = startY + (startHeight - lowestHeight);
        }
        newHeight = lowestHeight;
    }
    if (newHeight > MAX_H)
    {
        if (edge == ResizeEdge::Top || edge == ResizeEdge::TopLeft || edge == ResizeEdge::TopRight)
        {
            newY = startY + (startHeight - MAX_H);
        }
        newHeight = MAX_H;
    }

    return {.x = newX, .y = newY, .width = newWidth, .height = newHeight};
}

/// Pure geometry: given where the mouse was within a maximized window (as an offset of
/// startMouseGlobalX from the window's left edge, maximizedWindowX) and the maximized
/// window's width snapshot, compute the new left-edge X for the just-restored window
/// (restoredWidth wide) so the mouse ends up at the same proportional offset it had while
/// the window was maximized -- used when a drag that starts on a maximized window crosses
/// the un-maximize threshold. maximizedWindowWidth <= 0 (e.g. a compositor race during
/// window mapping) falls back to treating the mouse as horizontally centered (proportion
/// 0.5) rather than dividing by zero, which would produce NaN/Inf and make the int cast
/// below undefined behavior; the proportion is also clamped to [0, 1] so the mouse stays
/// within the restored window's horizontal span (between its left and right edges) even if
/// the captured position was outside the maximized window's bounds -- this bounds where the
/// mouse ends up relative to the restored window, not the restored window's absolute screen
/// position, which is unconstrained.
[[nodiscard]] inline auto computeRestoreFromMaximizedDragX(const int startMouseGlobalX,
                                                           const int maximizedWindowX,
                                                           const int maximizedWindowWidth,
                                                           const int restoredWidth) -> int
{
    const float xProportion =
        maximizedWindowWidth > 0
            ? std::clamp(static_cast<float>(startMouseGlobalX - maximizedWindowX) / static_cast<float>(maximizedWindowWidth), 0.0F, 1.0F)
            : 0.5F;
    return startMouseGlobalX - static_cast<int>(xProportion * static_cast<float>(restoredWidth));
}

/// Decision for the empty title-bar drag area specifically (once the point is known to be
/// in the title-bar row, not a resize border, and not a button). Pure/header-only, like
/// computeResizeGeometry above, so it's directly unit-testable without live SDL_Window or
/// TitleBarLayer state. See #744.
[[nodiscard]] inline auto computeTitleBarAreaHitTest(const bool isInControlArea, const bool isNativeWayland) -> SDL_HitTestResult
{
    if (isInControlArea)
    {
        return SDL_HITTEST_NORMAL;
    }
    return isNativeWayland ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

/// Full non-Windows hit-test decision tree, given already-queried SDL/TitleBarLayer state.
/// Pure/header-only, like computeResizeGeometry and computeTitleBarAreaHitTest above, so the
/// *ordering* itself is directly unit-testable without live SDL_Window/TitleBarLayer state: a
/// title-bar button can sit within resizeBorderThickness of a window edge (the close button's
/// rightmost pixels reach the window's right edge; the icon's top pixels sit inside the top
/// resize strip), so the control-area check below must run before any resize-border branch, or
/// SDL would consume the click as a resize instead of delivering it to the app (see #750 review).
[[nodiscard]] inline auto computeWindowHitTest(const float x,
                                               const float y,
                                               const int windowWidth,
                                               const int windowHeight,
                                               const float titleBarHeight,
                                               const float resizeBorderThickness,
                                               const bool isMaximized,
                                               const bool isInControlArea,
                                               const bool isNativeWayland) -> SDL_HitTestResult
{
    if (isInControlArea)
    {
        return SDL_HITTEST_NORMAL;
    }

    if (!isMaximized)
    {
        if (y >= static_cast<float>(windowHeight) - resizeBorderThickness)
        {
            if (x < resizeBorderThickness)
            {
                return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            }
            if (x >= static_cast<float>(windowWidth) - resizeBorderThickness)
            {
                return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            }
            return SDL_HITTEST_RESIZE_BOTTOM;
        }

        if (x < resizeBorderThickness)
        {
            if (y < resizeBorderThickness)
            {
                return SDL_HITTEST_RESIZE_TOPLEFT;
            }
            return SDL_HITTEST_RESIZE_LEFT;
        }
        if (x >= static_cast<float>(windowWidth) - resizeBorderThickness)
        {
            if (y < resizeBorderThickness)
            {
                return SDL_HITTEST_RESIZE_TOPRIGHT;
            }
            return SDL_HITTEST_RESIZE_RIGHT;
        }

        if (y < resizeBorderThickness)
        {
            return SDL_HITTEST_RESIZE_TOP;
        }
    }

    // titleBarHeight is an exclusive upper bound: ShellLayer::onRender() positions the
    // content window's top edge at exactly y == titleBarHeight, so that row already belongs
    // to content, not the title bar -- treat it as such here too, or native Wayland would
    // make it DRAGGABLE and steal its clicks from the content/tab area.
    if (y >= titleBarHeight)
    {
        return SDL_HITTEST_NORMAL;
    }

    return computeTitleBarAreaHitTest(/*isInControlArea=*/false, isNativeWayland);
}

/// Outcome of computeResizeCursorUpdate: what TitleBarLayer::updateResizeCursor() should do
/// with its cached hover edge and the SDL cursor for one frame.
struct ResizeCursorUpdate
{
    ResizeEdge resolvedEdge = ResizeEdge::None; ///< The edge this frame resolves to.
    bool updateCachedEdge = false;              ///< Whether the caller should cache resolvedEdge.
    bool applyCursor = false;                   ///< Whether the caller should call SDL_SetCursor (via applyCursorForEdge).
};

/// Pure decision for TitleBarLayer::updateResizeCursor(), given already-queried state. Pure
/// header-only, like the hit-test helpers above, so the policy -- active resize/drag vs.
/// WM/compositor-focus mismatch vs. real hover (including the state-unchanged fast path) --
/// is directly unit-testable without a live SDL_Window. See #699, #749, and the #750 review:
/// during a focus mismatch, the cached edge must be held and SDL_SetCursor must not be called
/// at all, not even to reapply the same cursor, or the WM/compositor's own cursor rendering
/// gets fought during an active border resize or title-bar drag.
///
/// isInteracting must be true for BOTH active resize and active drag, not just resize:
/// during a client-side drag, real hover sampling must not run, or the window-local mouse
/// coordinate transiently crossing into the resize-border zone as the window moves under the
/// drag would flip in a resize cursor mid-drag. resizeEdge is None during a drag (no resize
/// edge is active), which correctly resolves to no cursor override / a cache reset to None.
[[nodiscard]] inline auto computeResizeCursorUpdate(const bool isInteracting,
                                                    const ResizeEdge resizeEdge,
                                                    const bool focusMismatch,
                                                    const bool hoverSampleAvailable,
                                                    const ResizeEdge hoverEdge,
                                                    const ResizeEdge cachedEdge) -> ResizeCursorUpdate
{
    if (focusMismatch)
    {
        return {.resolvedEdge = cachedEdge, .updateCachedEdge = false, .applyCursor = false};
    }

    ResizeEdge edge = cachedEdge;
    bool updateCache = false;
    if (isInteracting)
    {
        edge = resizeEdge;
    }
    else if (hoverSampleAvailable)
    {
        edge = hoverEdge;
        updateCache = true;
    }
    // else: state unchanged (not resizing, no new hover sample) -- reuse cachedEdge so the
    // cursor-apply step below still runs every frame (ImGui may reset the cursor), even
    // though there is nothing new to cache.

    // Always (re)apply a non-None edge. Apply None only when leaving a non-None cached edge,
    // so ImGui-provided cursors (text inputs, splitters, etc.) are not overridden while the
    // pointer is away from window borders and nothing changed.
    const bool applyCursor = (edge != ResizeEdge::None) || (cachedEdge != ResizeEdge::None);
    return {.resolvedEdge = edge, .updateCachedEdge = updateCache, .applyCursor = applyCursor};
}

} // namespace App
