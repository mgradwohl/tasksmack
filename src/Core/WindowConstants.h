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

/// The longest the frame loop waits for an event while the window is hidden and draws no frames
/// (#3156; FramePacing::computeHiddenWait()). Restore, expose, input and quit events end the wait at
/// once, so this only bounds the work each wake does without frames: the shown-window cloak re-check
/// above (hence no longer than its interval), and the layers' onHiddenUpdate(), which takes Process
/// Details' watched samples before the process model's ring of them wraps (ShellPanels.cpp).
constexpr std::chrono::milliseconds WINDOW_HIDDEN_WAIT_TIMEOUT{1000};
static_assert(WINDOW_HIDDEN_WAIT_TIMEOUT <= WINDOW_CLOAK_RECHECK_INTERVAL);

/// The longest the frame loop waits while the window is cloaked (#3156). Nothing reports the cloak
/// ending, so each wake asks DWM again (Window::pollVisibility()): going back to the window's virtual
/// desktop draws within this, as it did at the hidden rate's 5 fps before. A wake that draws nothing
/// costs one DwmGetWindowAttribute() call.
constexpr std::chrono::milliseconds WINDOW_CLOAKED_POLL_INTERVAL{200};

} // namespace Core
