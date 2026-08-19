#pragma once

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

} // namespace Core
