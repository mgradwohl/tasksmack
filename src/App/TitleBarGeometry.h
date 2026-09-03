#pragma once

#include "Core/WindowConstants.h"

#include <SDL3/SDL_video.h>

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
[[nodiscard]] inline auto computeResizeGeometry(
    const ResizeEdge edge, const int startX, const int startY, const int startWidth, const int startHeight, const int dx, const int dy)
    -> WindowRect
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
    constexpr int MIN_W = Core::WINDOW_MIN_DIMENSION;
    constexpr int MAX_W = Core::WINDOW_MAX_DIMENSION;
    constexpr int MIN_H = Core::WINDOW_MIN_DIMENSION;
    constexpr int MAX_H = Core::WINDOW_MAX_DIMENSION;

    if (newWidth < MIN_W)
    {
        if (edge == ResizeEdge::Left || edge == ResizeEdge::TopLeft || edge == ResizeEdge::BottomLeft)
        {
            newX = startX + (startWidth - MIN_W);
        }
        newWidth = MIN_W;
    }
    if (newWidth > MAX_W)
    {
        if (edge == ResizeEdge::Left || edge == ResizeEdge::TopLeft || edge == ResizeEdge::BottomLeft)
        {
            newX = startX + (startWidth - MAX_W);
        }
        newWidth = MAX_W;
    }
    if (newHeight < MIN_H)
    {
        if (edge == ResizeEdge::Top || edge == ResizeEdge::TopLeft || edge == ResizeEdge::TopRight)
        {
            newY = startY + (startHeight - MIN_H);
        }
        newHeight = MIN_H;
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
