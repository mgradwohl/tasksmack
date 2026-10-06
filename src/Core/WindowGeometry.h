#pragma once

// Pure window-geometry decisions extracted from Window and the startup/shutdown code that persists
// the window's size, position and maximized state, so they can be unit-tested without a live SDL
// window or a particular monitor layout -- see CONTRIBUTING.md's "extract the pure decision logic
// into a small header" pattern (also used by Core/FramePacing.h and App/TitleBarGeometry.h).
//
// Every rectangle is in SDL's logical screen coordinates (the units of SDL_GetWindowPosition,
// SDL_GetWindowSize and SDL_GetDisplayUsableBounds), never physical pixels.

#include "Core/WindowConstants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

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

/// The window scale that goes with selectNormalGeometry()'s rectangle (#1168): the live scale when
/// not maximized, and when maximized the scale captured with the restore rectangle -- not the live
/// one, which measures the maximized window and may belong to another display or a later scale
/// setting. 0 (unknown) when maximized with no restore rectangle or no scale captured with it, so
/// the size is saved without a scale and restored as saved.
[[nodiscard]] constexpr auto selectNormalGeometryScale(bool maximized, bool hasRestoreRect, float restoreScale, float liveScale) noexcept
    -> float
{
    if (!maximized)
    {
        return liveScale;
    }
    return hasRestoreRect ? restoreScale : 0.0F;
}

/// Whether an OS-initiated maximize (SDL_EVENT_WINDOW_MAXIMIZED) should be replaced by the window's
/// own client-side maximize, the one the title-bar button uses (#1208).
///
/// A borderless window has no OS frame, so on the backends that maximize it client-side (Windows,
/// X11, XWayland) the OS's own maximize is wrong: on Windows, SDL answers WM_GETMINMAXINFO for a
/// borderless resizable window from the primary screen's metrics, which left the window a quarter
/// of a 175 % display with its title-bar buttons clipped. Window::isMaximized() also reads only the
/// tracked flag there, so the OS maximize went unnoticed: the title bar still offered Maximize,
/// which would then have recorded the OS-maximized rectangle as the restore target. Undoing it and
/// filling the current display's usable bounds instead fixes both. A native Wayland compositor
/// maximizes the borderless window correctly and Window tracks it through SDL_WINDOW_MAXIMIZED, and
/// a window with an OS frame is the OS's to maximize, so neither is adopted.
///
/// @param borderless         Whether the window is borderless (custom title bar).
/// @param clientSideBackend  Whether the backend maximizes borderless windows client-side
///                           (VideoBackend::supportsClientSideMaximize()).
/// @param usableBoundsKnown  Whether the current display's usable bounds can be read. Without them
///                           the client-side maximize itself falls back to SDL_MaximizeWindow(),
///                           whose own MAXIMIZED event must not be adopted again, or the two would
///                           undo each other on every event.
/// @param stillMaximized     Whether SDL_WINDOW_MAXIMIZED is still set when the event is handled.
/// @param minimized          Whether SDL_WINDOW_MINIMIZED is set when the event is handled. SDL
///                           events are queued, so the MAXIMIZED notification can be drained after
///                           a later OS restore or minimize already changed the window; adopting it
///                           then would undo that newer action, so only a window that is still
///                           maximized and not minimized is adopted (#1208).
[[nodiscard]] constexpr bool
shouldAdoptSystemMaximize(bool borderless, bool clientSideBackend, bool usableBoundsKnown, bool stillMaximized, bool minimized) noexcept
{
    return borderless && clientSideBackend && usableBoundsKnown && stillMaximized && !minimized;
}

/// How the window is maximized, as far as NormalGeometryTracker knows.
enum class MaximizeState : std::uint8_t
{
    /// Not maximized: the live geometry is the normal geometry.
    Normal,
    /// Maximized client-side by Window::maximize(): moved and sized to the display's usable bounds,
    /// with no OS maximized state (X11, XWayland and Windows, borderless). Only Window::restore()
    /// ends it; an OS "restored" notification is not about it.
    ClientSide,
    /// Maximized by the OS, window manager or compositor (SDL_WINDOW_MAXIMIZED is set): a system
    /// maximize, a native Wayland compositor maximize, or maximize()'s SDL_MaximizeWindow fallback.
    /// The OS can end it, which SDL reports as SDL_EVENT_WINDOW_RESTORED.
    System,
};

/// Tracks the window's normal (restored) rectangle through every maximize and restore, whoever
/// starts it (#1250).
///
/// The restore rectangle used to be recorded only when Window::maximize() ran. A maximize by the
/// window manager (an X11/XWayland shortcut or menu) or the compositor was never seen, so on exit
/// the maximized rectangle was saved as the normal one, and the next launch opened a screen-sized
/// normal window (#1121 by another route). A compositor restore was not seen either, so a later
/// compositor maximize kept the restore rectangle from before it, and the size the user had chosen
/// since was lost. The tracker therefore remembers the last geometry the window had while normal and
/// takes that as the restore target when a maximize from outside the app is reported.
///
/// Pure state: Window feeds it SDL's live geometry and flags. Every rectangle is in logical screen
/// coordinates; a scale of 0 means unknown.
class NormalGeometryTracker
{
  public:
    /// The window moved or resized (SDL_EVENT_WINDOW_MOVED / _RESIZED) to @p live. Recorded as the
    /// last normal geometry only when @p normalNow -- not maximized (by any route), minimized or
    /// fullscreen right now -- and the tracker does not hold a client-side maximize, which no SDL
    /// flag shows. SDL updates its flags before queuing the events, so a resize that is part of a
    /// maximize is handled with the maximized flag already set and is left out.
    constexpr void observe(const Rect& live, float scale, bool normalNow) noexcept
    {
        if (normalNow && m_State != MaximizeState::ClientSide && live.width > 0 && live.height > 0)
        {
            m_LastNormal = live;
            m_LastNormalScale = scale;
        }
    }

    /// Window::maximize() is maximizing the window, @p how. When @p normalNow the window is still at
    /// its normal rectangle @p live, which becomes the restore target; otherwise it is already
    /// maximized, and the restore target found then is kept (a second maximize must not replace it
    /// with the maximized rectangle). If it is maximized although the tracker had not heard of it --
    /// a compositor maximize whose event is still queued -- the last normal geometry is taken, as
    /// systemMaximized() would.
    constexpr void maximizing(const Rect& live, float scale, bool normalNow, MaximizeState how) noexcept
    {
        if (normalNow)
        {
            observe(live, scale, true);
            captureRestoreTarget(live.width > 0 && live.height > 0 ? std::optional<Rect>{live} : std::nullopt, scale);
        }
        else if (m_State == MaximizeState::Normal)
        {
            captureRestoreTarget(m_LastNormal, m_LastNormalScale);
        }
        m_State = how;
    }

    /// SDL reported SDL_EVENT_WINDOW_MAXIMIZED and the window was not adopted into a client-side
    /// maximize. @p stillMaximized and @p minimized are the live flags when the event is handled:
    /// events are queued, so a MAXIMIZED can be drained after a later restore or minimize, and only a
    /// window that is still maximized and not minimized is taken as maximized (as with
    /// shouldAdoptSystemMaximize()). The restore target is the last normal geometry, unless the
    /// window was already maximized, whose restore target is kept.
    constexpr void systemMaximized(bool stillMaximized, bool minimized) noexcept
    {
        if (!stillMaximized || minimized || m_State != MaximizeState::Normal)
        {
            return;
        }
        captureRestoreTarget(m_LastNormal, m_LastNormalScale);
        m_State = MaximizeState::System;
    }

    /// SDL reported SDL_EVENT_WINDOW_RESTORED. Ends a System maximize when the live flags say the
    /// window is no longer maximized and is not minimized. Ignored otherwise: SDL also sends it when a
    /// minimized window comes back (still maximized, perhaps), when a queued restore is drained after
    /// a newer maximize, and when Window::adoptSystemMaximize() undoes an OS maximize on its way to a
    /// ClientSide one, which this must not end.
    constexpr void systemRestored(bool stillMaximized, bool minimized) noexcept
    {
        if (stillMaximized || minimized || m_State != MaximizeState::System)
        {
            return;
        }
        restored();
    }

    /// Window::restore() returned the window to its normal rectangle: the restore target is spent.
    constexpr void restored() noexcept
    {
        m_State = MaximizeState::Normal;
        m_RestoreTarget.reset();
        m_RestoreScale = 0.0F;
    }

    [[nodiscard]] constexpr auto state() const noexcept -> MaximizeState
    {
        return m_State;
    }

    [[nodiscard]] constexpr bool isMaximized() const noexcept
    {
        return m_State != MaximizeState::Normal;
    }

    /// The rectangle the window returns to when restored, while maximized and it is known.
    [[nodiscard]] constexpr auto restoreTarget() const noexcept -> const std::optional<Rect>&
    {
        return m_RestoreTarget;
    }

    /// The window scale restoreTarget() was measured at (#1168); 0 when unknown.
    [[nodiscard]] constexpr auto restoreScale() const noexcept -> float
    {
        return m_RestoreScale;
    }

  private:
    constexpr void captureRestoreTarget(const std::optional<Rect>& target, float scale) noexcept
    {
        m_RestoreTarget = target;
        m_RestoreScale = target.has_value() ? scale : 0.0F;
    }

    MaximizeState m_State = MaximizeState::Normal;
    std::optional<Rect> m_LastNormal;
    float m_LastNormalScale = 0.0F;
    std::optional<Rect> m_RestoreTarget;
    float m_RestoreScale = 0.0F;
};

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

/// The display a window at @p rect is reachable on (isReachableOn); among several, the one holding
/// most of it. std::nullopt when it is reachable on none.
[[nodiscard]] constexpr auto reachableDisplayIndex(const Rect& rect, std::span<const Rect> displays, int minVisible)
    -> std::optional<std::size_t>
{
    std::optional<std::size_t> best;
    std::int64_t bestArea = -1;
    for (std::size_t i = 0; i < displays.size(); ++i)
    {
        const Rect& display = displays[i];
        if (!isReachableOn(rect, display, minVisible))
        {
            continue;
        }
        const std::int64_t area =
            spanOverlap(rect.x, rect.width, display.x, display.width) * spanOverlap(rect.y, rect.height, display.y, display.height);
        if (area > bestArea)
        {
            bestArea = area;
            best = i;
        }
    }
    return best;
}

/// The display fitRectToDisplays() puts a window at @p saved on: the one it is reachable on, or else
/// the primary display (out of range means the first). std::nullopt only when no display is known.
[[nodiscard]] constexpr auto targetDisplayIndex(const Rect& saved, std::span<const Rect> displays, std::size_t primaryIndex, int minVisible)
    -> std::optional<std::size_t>
{
    if (displays.empty())
    {
        return std::nullopt;
    }
    if (const auto reachable = reachableDisplayIndex(saved, displays, minVisible); reachable.has_value())
    {
        return reachable;
    }
    return primaryIndex < displays.size() ? primaryIndex : 0;
}

/// Largest window scale (window units per 96-DPI point) accepted from a saved config. Windows tops
/// out at 500 %; anything far beyond that is a corrupt value, not a display.
inline constexpr float MAX_WINDOW_SCALE = 16.0F;

/// Whether @p scale is a usable window scale: positive, finite and not absurd. False for NaN.
[[nodiscard]] constexpr bool isUsableWindowScale(float scale) noexcept
{
    return scale > 0.0F && scale <= MAX_WINDOW_SCALE;
}

/// The UI scale in window units: SDL's display scale divided by the window's pixel density.
///
/// On Windows and X11 window coordinates are physical pixels (density 1), so this is the display's
/// scale (1.75 at 175 %). On native Wayland and macOS the window is created with
/// SDL_WINDOW_HIGH_PIXEL_DENSITY, window coordinates are already logical and the pixel density
/// carries the scale, so this is 1 (#1096). A density that is not a usable number (0 on failure,
/// NaN) falls back to the display scale alone. UI::windowUnitScale() forwards here.
[[nodiscard]] inline float windowUnitScale(float displayScale, float pixelDensity) noexcept
{
    if (!std::isfinite(pixelDensity) || pixelDensity <= 0.0F)
    {
        return displayScale;
    }
    return displayScale / pixelDensity;
}

/// Convert a saved window size to window units on the display the window is restored to (#1168).
///
/// On Windows the window's size is in physical pixels, so a 1280 x 720 window saved on a 100 %
/// display looked half as big when reopened on a 200 % one, and a size saved at 200 % reopened
/// double-sized at 100 %. The size is therefore saved together with the window scale it was
/// measured at, and scaled by @p targetScale / @p savedScale on restore so it keeps its apparent
/// size. A config written before the scale was saved has no @p savedScale and is restored as it
/// was, unchanged; so is any size when either scale is unusable. The result is kept within
/// [WINDOW_MIN_DIMENSION, WINDOW_MAX_DIMENSION].
///
/// @param width        Saved width, in window units at @p savedScale.
/// @param height       Saved height, in window units at @p savedScale.
/// @param savedScale   Window scale the size was saved at (windowUnitScale()), if known.
/// @param targetScale  Window scale of the display the window is restored to.
/// @return {width, height} in window units at @p targetScale.
[[nodiscard]] constexpr auto rescaleWindowSize(int width, int height, std::optional<float> savedScale, float targetScale)
    -> std::pair<int, int>
{
    if (!savedScale.has_value() || !isUsableWindowScale(*savedScale) || !isUsableWindowScale(targetScale))
    {
        return {width, height};
    }
    const double ratio = static_cast<double>(targetScale) / static_cast<double>(*savedScale);
    const auto convert = [ratio](int value)
    {
        const double scaled = (static_cast<double>(value) * ratio) + 0.5;
        const double clamped = std::clamp(scaled, static_cast<double>(WINDOW_MIN_DIMENSION), static_cast<double>(WINDOW_MAX_DIMENSION));
        return static_cast<int>(clamped);
    };
    return {convert(width), convert(height)};
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

    if (const std::optional<std::size_t> best = reachableDisplayIndex(saved, displays, minVisible); best.has_value())
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
