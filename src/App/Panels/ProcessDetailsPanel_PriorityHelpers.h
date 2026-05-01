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

namespace App::Detail
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

// Visual styling constants
inline constexpr float PRIORITY_SLIDER_CORNER_RADIUS = 2.0F;    // Border rounding for slider bar
inline constexpr float PRIORITY_BADGE_CORNER_RADIUS = 4.0F;     // Border rounding for value badge
inline constexpr float PRIORITY_THUMB_OUTLINE_THICKNESS = 2.0F; // Outline width for slider thumb
inline constexpr float PRIORITY_LABEL_PADDING = 8.0F;           // Padding between High/Low labels and slider

// Nice value range - imported from Domain for consistency (DRY principle)
inline constexpr int32_t NICE_MIN = Domain::Priority::MIN_NICE;
inline constexpr int32_t NICE_MAX = Domain::Priority::MAX_NICE;
inline constexpr int32_t NICE_RANGE = NICE_MAX - NICE_MIN; // 39

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Interpolate color based on nice value (-20 to 19)
 *
 * Returns a gradient color:
 * - nice -20: high priority color (default: red/orange)
 * - nice 0: normal priority color (default: green)
 * - nice 19: low priority color (default: blue/gray)
 *
 * Note: Returns ImU32 directly (rather than ImVec4) because all call sites
 * use the packed format for ImDrawList operations. This avoids redundant
 * ImVec4->ImU32 conversions at each call site.
 *
 * @param nice The nice value (-20 to 19)
 * @param high Color for high-priority end (nice == -20)
 * @param normal Color for normal priority (nice == 0)
 * @param low Color for low-priority end (nice == 19)
 * @return ImU32 The interpolated color in packed RGBA format
 */
[[nodiscard]] inline auto getNiceColor(int32_t nice, const ImVec4& high, const ImVec4& normal, const ImVec4& low) -> ImU32
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
        r = high.x + (t * (normal.x - high.x));
        g = high.y + (t * (normal.y - high.y));
        b = high.z + (t * (normal.z - high.z));
    }
    else
    {
        // Interpolate between green (normal) and blue (low priority)
        // nice = 0  -> t = 0.0 (green)
        // nice = 19 -> t = 1.0 (blue)
        const float t = static_cast<float>(nice) / static_cast<float>(NICE_MAX);
        r = normal.x + (t * (low.x - normal.x));
        g = normal.y + (t * (low.y - normal.y));
        b = normal.z + (t * (low.z - normal.z));
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

} // namespace App::Detail
