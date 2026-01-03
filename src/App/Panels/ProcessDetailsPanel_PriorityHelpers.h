#pragma once

// This header exposes priority slider helper functions for testing and
// internal use. These were extracted from ProcessDetailsPanel::renderActions()
// to improve testability and code organization.

#include "Domain/PriorityConfig.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace App::detail
{

// =============================================================================
// Priority Slider Constants
// =============================================================================

// Slider dimensions
inline constexpr float PRIORITY_SLIDER_WIDTH = 400.0F;
inline constexpr float PRIORITY_SLIDER_HEIGHT = 12.0F;
inline constexpr float PRIORITY_BADGE_HEIGHT = 24.0F;
inline constexpr float PRIORITY_BADGE_ARROW_SIZE = 8.0F;
inline constexpr float PRIORITY_GRADIENT_SEGMENTS = 40.0F;

// Nice value range - imported from Domain for consistency (DRY principle)
inline constexpr int32_t NICE_MIN = Domain::Priority::MIN_NICE;
inline constexpr int32_t NICE_MAX = Domain::Priority::MAX_NICE;
inline constexpr int32_t NICE_RANGE = NICE_MAX - NICE_MIN; // 39

// Color anchors for gradient (at nice values -20, 0, 19)
// High priority (nice -20) = red/orange
// Normal priority (nice 0) = green
// Low priority (nice 19) = blue/gray
inline constexpr std::array<float, 3> PRIORITY_COLOR_HIGH = {1.0F, 0.3F, 0.2F};   // Red
inline constexpr std::array<float, 3> PRIORITY_COLOR_NORMAL = {0.5F, 0.8F, 0.2F}; // Green
inline constexpr std::array<float, 3> PRIORITY_COLOR_LOW = {0.4F, 0.4F, 0.8F};    // Blue

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Interpolate color based on nice value (-20 to 19)
 *
 * Returns a gradient color:
 * - nice -20: Red (high priority, uses more CPU)
 * - nice 0: Green (normal priority)
 * - nice 19: Blue (low priority, yields CPU)
 *
 * @param nice The nice value (-20 to 19)
 * @return ImU32 The interpolated color
 */
[[nodiscard]] inline auto getNiceColor(int32_t nice) -> ImU32
{
    // Clamp nice value to valid range
    nice = std::clamp(nice, NICE_MIN, NICE_MAX);

    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;

    if (nice <= 0)
    {
        // Interpolate between red (high priority) and green (normal)
        // nice = -20 -> t = 0.0 (red)
        // nice = 0   -> t = 1.0 (green)
        const float t = static_cast<float>(nice - NICE_MIN) / static_cast<float>(-NICE_MIN);
        r = PRIORITY_COLOR_HIGH[0] + (t * (PRIORITY_COLOR_NORMAL[0] - PRIORITY_COLOR_HIGH[0]));
        g = PRIORITY_COLOR_HIGH[1] + (t * (PRIORITY_COLOR_NORMAL[1] - PRIORITY_COLOR_HIGH[1]));
        b = PRIORITY_COLOR_HIGH[2] + (t * (PRIORITY_COLOR_NORMAL[2] - PRIORITY_COLOR_HIGH[2]));
    }
    else
    {
        // Interpolate between green (normal) and blue (low priority)
        // nice = 0  -> t = 0.0 (green)
        // nice = 19 -> t = 1.0 (blue)
        const float t = static_cast<float>(nice) / static_cast<float>(NICE_MAX);
        r = PRIORITY_COLOR_NORMAL[0] + (t * (PRIORITY_COLOR_LOW[0] - PRIORITY_COLOR_NORMAL[0]));
        g = PRIORITY_COLOR_NORMAL[1] + (t * (PRIORITY_COLOR_LOW[1] - PRIORITY_COLOR_NORMAL[1]));
        b = PRIORITY_COLOR_NORMAL[2] + (t * (PRIORITY_COLOR_LOW[2] - PRIORITY_COLOR_NORMAL[2]));
    }

    // Use std::lround for accurate color representation (avoids truncation)
    return IM_COL32(static_cast<int>(std::lround(r * 255.0F)),
                    static_cast<int>(std::lround(g * 255.0F)),
                    static_cast<int>(std::lround(b * 255.0F)),
                    255);
}

/**
 * @brief Get the normalized position (0.0 to 1.0) for a nice value
 *
 * @param nice The nice value (-20 to 19)
 * @return float Position from 0.0 (nice -20) to 1.0 (nice 19)
 */
[[nodiscard]] inline auto getNicePosition(int32_t nice) -> float
{
    nice = std::clamp(nice, NICE_MIN, NICE_MAX);
    return static_cast<float>(nice - NICE_MIN) / static_cast<float>(NICE_RANGE);
}

/**
 * @brief Get the nice value from a normalized position (0.0 to 1.0)
 *
 * @param position Normalized position (0.0 to 1.0)
 * @return int32_t The corresponding nice value (-20 to 19)
 */
[[nodiscard]] inline auto getNiceFromPosition(float position) -> int32_t
{
    position = std::clamp(position, 0.0F, 1.0F);
    return NICE_MIN + static_cast<int32_t>(std::round(position * static_cast<float>(NICE_RANGE)));
}

// Note: For priority labels, use Domain::Priority::getPriorityLabel() from PriorityConfig.h
// to maintain consistency across the application.

} // namespace App::detail
