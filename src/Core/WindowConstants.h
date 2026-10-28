#pragma once

#include <chrono>

namespace Core
{

/// Minimum window dimension (width or height) in pixels.
/// Use at all window-size clamp sites (main.cpp, UserConfig.cpp, custom resize)
/// to maintain a single source of truth.
constexpr int WINDOW_MIN_DIMENSION = 200;

/// Maximum window dimension (width or height) in pixels.
/// Use at all window-size clamp sites (main.cpp, UserConfig.cpp, custom resize)
/// to maintain a single source of truth.
constexpr int WINDOW_MAX_DIMENSION = 16'384;

/// How often the frame loop asks DWM whether the shown window has been cloaked, for example moved
/// off the current virtual desktop, on Windows (#3152). Windows sends no message for it; activation
/// and focus changes re-check at once, so this only bounds how long a window cloaked without them
/// keeps the normal rate. A cloaked window is re-checked on every frame instead (WindowVisibility::
/// cloakRecheckDue()). Wall-clock time, never a frame count.
constexpr std::chrono::milliseconds WINDOW_CLOAK_RECHECK_INTERVAL{1000};

} // namespace Core
