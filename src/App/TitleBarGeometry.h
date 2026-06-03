#pragma once

#include "Core/Window.h"

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

/// Pure geometry function: given the active resize edge and the mouse delta from drag
/// start, compute the clamped window rect. No member state — pure inputs/outputs.
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

    return {newX, newY, newWidth, newHeight};
}

} // namespace App
