#pragma once

// Pure decisions behind Window::isHidden() (#3152): what counts as a hidden window, and how the
// Windows messages that report the window's visibility change it. Free of <windows.h>, so they are
// tested on every platform; Window.cpp checks the Win32 values mirrored here against the real ones.
// See CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (also used by
// Core/FramePacing.h and Core/WindowGeometry.h).
//
// SDL 3.4 reports SDL_WINDOW_OCCLUDED only on Cocoa, Wayland and X11, so on Windows the window's
// subclass procedure gathers these signals itself. They slow drawing only: sampling keeps the
// Settings interval whatever the window's state (#2731, #1125).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace Core::WindowVisibility
{

/// The Windows signals that put the window out of sight without minimizing it. All false
/// elsewhere, where SDL_WINDOW_OCCLUDED says what the windowing system knows.
struct Signals
{
    bool cloaked = false;             ///< DWMWA_CLOAKED: on another virtual desktop, among other cases
    bool sessionLocked = false;       ///< WTS_SESSION_LOCK until WTS_SESSION_UNLOCK
    bool sessionDisconnected = false; ///< A console or remote disconnect until the next connect
    bool displayOff = false;          ///< The session's display is powered off

    [[nodiscard]] constexpr bool operator==(const Signals&) const noexcept = default;
};

/// Whether any of @p signals hides the window.
[[nodiscard]] constexpr bool anyHides(const Signals& signals) noexcept
{
    return signals.cloaked || signals.sessionLocked || signals.sessionDisconnected || signals.displayOff;
}

/// Whether the window is hidden, so the frame loop draws at the hidden rate (#1125): minimized,
/// occluded as SDL reports it, or out of sight by one of @p signals.
[[nodiscard]] constexpr bool isHidden(bool minimized, bool occluded, const Signals& signals) noexcept
{
    return minimized || occluded || anyHides(signals);
}

/// How a hidden window will be shown again, which decides how the frame loop waits while it is
/// hidden (#3156).
enum class HiddenState : std::uint8_t
{
    Shown,  ///< Not hidden: isHidden() is false
    Hidden, ///< Minimized, occluded, locked, disconnected or display off: its end comes with an event
    /// Cloaked: DWM sends nothing when the cloak ends, so the loop must keep asking
    /// (Window::pollVisibility()) to notice, for example, a return to the window's virtual desktop.
    Cloaked,
    /// Minimized on native Wayland and nothing else: xdg-shell reports no minimize, so SDL sets the
    /// flag when the app asks for one and clears it only at the next activating configure. A window
    /// shown again without being activated keeps the flag, so the loop keeps drawing at the hidden
    /// rate rather than not at all.
    MinimizedUnconfirmed,
};

/// The HiddenState of a window that is @p minimized and @p occluded as SDL reports it, with
/// @p signals. @p minimizeEndReported is whether the backend reports the end of a minimize: false
/// on native Wayland only. Hidden whenever isHidden() is, and Shown otherwise. A cloak comes first:
/// whatever else ends with an event, the cloak still needs polling after it.
[[nodiscard]] constexpr HiddenState classifyHidden(bool minimized, bool minimizeEndReported, bool occluded, const Signals& signals) noexcept
{
    if (signals.cloaked)
    {
        return HiddenState::Cloaked;
    }
    if (occluded || anyHides(signals))
    {
        return HiddenState::Hidden;
    }
    if (minimized)
    {
        return minimizeEndReported ? HiddenState::Hidden : HiddenState::MinimizedUnconfirmed;
    }
    return HiddenState::Shown;
}

// ---- Session changes: WM_WTSSESSION_CHANGE ----

/// WM_WTSSESSION_CHANGE's wParam values this module reads (<winuser.h>).
inline constexpr std::uintptr_t WIN32_WTS_CONSOLE_CONNECT = 0x1U;
inline constexpr std::uintptr_t WIN32_WTS_CONSOLE_DISCONNECT = 0x2U;
inline constexpr std::uintptr_t WIN32_WTS_REMOTE_CONNECT = 0x3U;
inline constexpr std::uintptr_t WIN32_WTS_REMOTE_DISCONNECT = 0x4U;
inline constexpr std::uintptr_t WIN32_WTS_SESSION_LOCK = 0x7U;
inline constexpr std::uintptr_t WIN32_WTS_SESSION_UNLOCK = 0x8U;

/// What a WM_WTSSESSION_CHANGE means for the window's visibility.
enum class SessionChange : std::uint8_t
{
    Other, ///< Logon, logoff, remote control, ...: changes nothing
    Lock,
    Unlock,
    Connect,    ///< The console or a remote client connected to the session
    Disconnect, ///< The console or the remote client disconnected from it
};

/// The change in a WM_WTSSESSION_CHANGE's wParam.
[[nodiscard]] constexpr auto decodeSessionChange(std::uintptr_t wParam) noexcept -> SessionChange
{
    switch (wParam)
    {
    case WIN32_WTS_SESSION_LOCK:
        return SessionChange::Lock;
    case WIN32_WTS_SESSION_UNLOCK:
        return SessionChange::Unlock;
    case WIN32_WTS_CONSOLE_CONNECT:
    case WIN32_WTS_REMOTE_CONNECT:
        return SessionChange::Connect;
    case WIN32_WTS_CONSOLE_DISCONNECT:
    case WIN32_WTS_REMOTE_DISCONNECT:
        return SessionChange::Disconnect;
    default:
        return SessionChange::Other;
    }
}

/// @p signals after @p change. An unlock also ends a disconnect: someone is using the session, so a
/// connect it never saw can't leave the window hidden.
[[nodiscard]] constexpr auto applySessionChange(Signals signals, SessionChange change) noexcept -> Signals
{
    switch (change)
    {
    case SessionChange::Lock:
        signals.sessionLocked = true;
        break;
    case SessionChange::Unlock:
        signals.sessionLocked = false;
        signals.sessionDisconnected = false;
        break;
    case SessionChange::Connect:
        signals.sessionDisconnected = false;
        break;
    case SessionChange::Disconnect:
        signals.sessionDisconnected = true;
        break;
    case SessionChange::Other:
        break;
    }
    return signals;
}

// ---- Display state: WM_POWERBROADCAST PBT_POWERSETTINGCHANGE ----

/// WM_POWERBROADCAST's wParam for a power-setting notification (<winuser.h>).
inline constexpr std::uintptr_t WIN32_PBT_POWERSETTINGCHANGE = 0x8013U;
/// The display-state notification's DWORD values (MONITOR_DISPLAY_STATE, <winnt.h>).
inline constexpr std::uint32_t WIN32_POWER_MONITOR_OFF = 0U;
inline constexpr std::uint32_t WIN32_POWER_MONITOR_ON = 1U;
inline constexpr std::uint32_t WIN32_POWER_MONITOR_DIM = 2U;

/// The display state a power-setting notification reports.
enum class DisplayState : std::uint8_t
{
    Unknown, ///< Too short to hold the state, or a value Windows doesn't document: changes nothing
    Off,
    On,
    Dimmed, ///< Dimmed, but still showing the window
};

/// The display state in a POWERBROADCAST_SETTING's Data: a DWORD in the machine's byte order.
[[nodiscard]] inline auto decodeDisplayState(std::span<const std::byte> data) noexcept -> DisplayState
{
    std::uint32_t value = 0;
    if (data.size() < sizeof(value))
    {
        return DisplayState::Unknown;
    }
    std::memcpy(&value, data.data(), sizeof(value));
    switch (value)
    {
    case WIN32_POWER_MONITOR_OFF:
        return DisplayState::Off;
    case WIN32_POWER_MONITOR_ON:
        return DisplayState::On;
    case WIN32_POWER_MONITOR_DIM:
        return DisplayState::Dimmed;
    default:
        return DisplayState::Unknown;
    }
}

/// @p signals after the display reported @p state. A dimmed display still shows the window.
[[nodiscard]] constexpr auto applyDisplayState(Signals signals, DisplayState state) noexcept -> Signals
{
    switch (state)
    {
    case DisplayState::Off:
        signals.displayOff = true;
        break;
    case DisplayState::On:
    case DisplayState::Dimmed:
        signals.displayOff = false;
        break;
    case DisplayState::Unknown:
        break;
    }
    return signals;
}

// ---- Cloaking: DWMWA_CLOAKED ----

/// Whether the frame loop should ask DWM again whether the window is cloaked. Windows sends no
/// message when a window is cloaked or uncloaked, so besides re-checking on activation and focus
/// changes the loop re-checks every @p interval while the window is shown, and on every frame
/// while it is cloaked: the hidden rate's frames are few, and going back to the window's desktop
/// must bring the normal rate back at once.
[[nodiscard]] constexpr bool
cloakRecheckDue(bool cloaked, std::chrono::steady_clock::duration sinceLastCheck, std::chrono::steady_clock::duration interval) noexcept
{
    return cloaked || sinceLastCheck >= interval;
}

} // namespace Core::WindowVisibility
