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

    if (y > titleBarHeight)
    {
        return SDL_HITTEST_NORMAL;
    }

    return computeTitleBarAreaHitTest(/*isInControlArea=*/false, isNativeWayland);
}

} // namespace App
