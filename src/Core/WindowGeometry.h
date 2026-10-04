#pragma once

// Pure window-geometry decisions extracted from Window and the startup/shutdown code that persists
// the window's size, position and maximized state, so they can be unit-tested without a live SDL
// window or a particular monitor layout -- see CONTRIBUTING.md's "extract the pure decision logic
// into a small header" pattern (also used by Core/FramePacing.h and App/TitleBarGeometry.h).
//
// Every rectangle is in SDL's logical screen coordinates (the units of SDL_GetWindowPosition,
// SDL_GetWindowSize and SDL_GetDisplayUsableBounds), never physical pixels.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Core::WindowGeometry
{

/// A window or display rectangle: top-left corner plus size.
struct Rect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const Rect&) const = default;
};

/// How much of a restored window has to be on some display for it to count as reachable, in
/// logical pixels. The custom title bar is the only place the borderless window can be grabbed, so
/// this much of its top edge must land on a display: enough to see it and drag it back (#1128).
inline constexpr int MIN_VISIBLE_EXTENT = 64;

/// The geometry to persist as the window's normal (restored) rectangle.
///
/// While the window is maximized its live size and position are the maximized ones. Saving those
/// and then maximizing again on the next launch makes the maximized rectangle the restore target,
/// so Restore did nothing until the window was dragged by hand (#1121). The rectangle the window
/// returns to is saved instead.
///
/// @param maximized    Whether the window is maximized right now.
/// @param current      Its live position and size.
/// @param restoreRect  The rectangle it was in before it was maximized, if that is known.
/// @return @p current when not maximized; @p restoreRect when maximized and it is a real
///         rectangle; std::nullopt when maximized with no usable restore rectangle, meaning the
///         previously saved normal geometry should be left as it is.
[[nodiscard]] constexpr auto selectNormalGeometry(bool maximized, const Rect& current, const std::optional<Rect>& restoreRect)
    -> std::optional<Rect>
{
    if (!maximized)
    {
        return current;
    }
    if (restoreRect.has_value() && restoreRect->width > 0 && restoreRect->height > 0)
    {
        return restoreRect;
    }
    return std::nullopt;
}

/// Length of the overlap of the half-open spans [aStart, aStart + aLength) and
/// [bStart, bStart + bLength), or 0 when they do not overlap. Computed in 64 bits so extreme saved
/// coordinates cannot overflow.
[[nodiscard]] constexpr auto spanOverlap(int aStart, int aLength, int bStart, int bLength) -> std::int64_t
{
    const std::int64_t start = std::max<std::int64_t>(aStart, bStart);
    const std::int64_t end =
        std::min<std::int64_t>(static_cast<std::int64_t>(aStart) + aLength, static_cast<std::int64_t>(bStart) + bLength);
    return std::max<std::int64_t>(0, end - start);
}

/// Whether a window at @p rect can be reached on @p display: its top edge (where the title bar is)
/// lies on the display with at least @p minVisible of the display below it, and at least
/// @p minVisible of its width overlaps the display horizontally.
[[nodiscard]] constexpr bool isReachableOn(const Rect& rect, const Rect& display, int minVisible)
{
    if (display.width <= 0 || display.height <= 0)
    {
        return false;
    }
    const int neededWidth = std::min(minVisible, std::max(rect.width, 1));
    const int neededHeight = std::min(minVisible, display.height);
    const bool topEdgeOnDisplay = static_cast<std::int64_t>(rect.y) >= display.y &&
                                  static_cast<std::int64_t>(rect.y) + neededHeight <= static_cast<std::int64_t>(display.y) + display.height;
    return topEdgeOnDisplay && spanOverlap(rect.x, rect.width, display.x, display.width) >= neededWidth;
}

/// Fit a restored window rectangle to the connected displays (#1128).
///
/// A saved position can belong to a monitor that is no longer connected, and the borderless window
/// has no OS title bar to grab, so applied as-is it can open entirely off-screen. This keeps the
/// saved position whenever the window's title-bar edge is reachable on some display, and otherwise
/// centres the window on the primary display. Either way the size is shrunk, never grown, to fit
/// the display the window ends up on.
///
/// @param saved         The saved rectangle (position and normal size).
/// @param displays      Each connected display's usable bounds (SDL_GetDisplayUsableBounds).
/// @param primaryIndex  Index of the primary display in @p displays; out of range means the first.
/// @param minVisible    How much of the window must be on a display to count as reachable.
/// @return The rectangle to apply; @p saved unchanged when no display is known.
[[nodiscard]] constexpr auto fitRectToDisplays(const Rect& saved, std::span<const Rect> displays, std::size_t primaryIndex, int minVisible)
    -> Rect
{
    if (displays.empty())
    {
        return saved;
    }

    // Prefer a display the saved rectangle is already reachable on; among several, the one holding
    // most of it.
    std::optional<std::size_t> best;
    std::int64_t bestArea = -1;
    for (std::size_t i = 0; i < displays.size(); ++i)
    {
        const Rect& display = displays[i];
        if (!isReachableOn(saved, display, minVisible))
        {
            continue;
        }
        const std::int64_t area =
            spanOverlap(saved.x, saved.width, display.x, display.width) * spanOverlap(saved.y, saved.height, display.y, display.height);
        if (area > bestArea)
        {
            bestArea = area;
            best = i;
        }
    }

    if (best.has_value())
    {
        const Rect& display = displays[*best];
        Rect fitted = saved;
        fitted.width = std::min(saved.width, display.width);
        fitted.height = std::min(saved.height, display.height);
        // Shrinking keeps the origin, so a window reachable only by its far edge (mostly off the left
        // of the display) can lose that edge: {-2936, 100, 3000, 720} on a 1920-wide display would
        // become {-2936, 100, 1920, 720}, entirely off-screen. Then move it just far enough to lie
        // on the display, keeping as much of the saved position as fits.
        if (!isReachableOn(fitted, display, minVisible))
        {
            const auto clampAxis = [](int pos, int length, int start, int span)
            {
                const std::int64_t lo = start;
                const std::int64_t hi = static_cast<std::int64_t>(start) + span - length; // >= lo: length <= span
                return static_cast<int>(std::clamp<std::int64_t>(pos, lo, hi));
            };
            fitted.x = clampAxis(fitted.x, fitted.width, display.x, display.width);
            fitted.y = clampAxis(fitted.y, fitted.height, display.y, display.height);
        }
        return fitted;
    }

    const Rect& primary = displays[primaryIndex < displays.size() ? primaryIndex : 0];
    Rect centred;
    centred.width = std::min(saved.width, primary.width);
    centred.height = std::min(saved.height, primary.height);
    centred.x = primary.x + ((primary.width - centred.width) / 2);
    centred.y = primary.y + ((primary.height - centred.height) / 2);
    return centred;
}

} // namespace Core::WindowGeometry
