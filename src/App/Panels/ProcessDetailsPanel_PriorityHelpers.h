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

// Slider geometry, in ems (#938).
//
// These were eight fixed pixel sizes, so the control ignored both the Font Size setting and the
// display's density: at Even Huger the nice value was taller than the badge drawn around it, and at
// Small the whole control dwarfed its own labels. One em is ImGui::GetFontSize(), which already
// accounts for both -- see UI/DialogMetrics.h for why that is the right unit.
//
// Each multiple was derived from the pixel size it replaces against the reference em of 32/3 px
// (App::REFERENCE_EM_PX: the Medium preset's 8pt body font on a 1.0 display scale), so the slider
// renders unchanged at the reference configuration and scales from there.
inline constexpr float PRIORITY_SLIDER_WIDTH_EM = 37.5F;              // 400px at the reference em
inline constexpr float PRIORITY_SLIDER_HEIGHT_EM = 1.125F;            // 12px
inline constexpr float PRIORITY_BADGE_HEIGHT_EM = 2.25F;              // 24px; the text inside is 1 em tall
inline constexpr float PRIORITY_BADGE_ARROW_SIZE_EM = 0.75F;          // 8px
inline constexpr float PRIORITY_SLIDER_CORNER_RADIUS_EM = 0.1875F;    // 2px, border rounding for slider bar
inline constexpr float PRIORITY_BADGE_CORNER_RADIUS_EM = 0.375F;      // 4px, border rounding for value badge
inline constexpr float PRIORITY_THUMB_OUTLINE_THICKNESS_EM = 0.1875F; // 2px, outline width for slider thumb
inline constexpr float PRIORITY_LABEL_PADDING_EM = 0.75F;             // 8px, between High/Low labels and slider
inline constexpr float PRIORITY_APPLY_BUTTON_MIN_EM = 11.25F;         // 120px, floor on the Apply button

/// Narrowest the track may be squeezed to when the panel is too narrow for the authored width.
/// Below this the 40 nice steps are too close together to pick between with a pointer.
inline constexpr float PRIORITY_SLIDER_MIN_WIDTH_EM = 10.0F;

/// Thumb radius as a fraction of the track height, so the thumb overhangs the track slightly.
inline constexpr float PRIORITY_THUMB_RADIUS_FRACTION = 0.6F;

/// Thinnest the thumb outline may get. Below a pixel it antialiases into a faint smudge.
inline constexpr float PRIORITY_THUMB_OUTLINE_MIN_PX = 1.0F;

// A count, not a size: the number of colour steps in the gradient, independent of the font.
inline constexpr float PRIORITY_GRADIENT_SEGMENTS = 40.0F;

// Nice value range - imported from Domain for consistency (DRY principle)
inline constexpr int32_t NICE_MIN = Domain::Priority::MIN_NICE;
inline constexpr int32_t NICE_MAX = Domain::Priority::MAX_NICE;
inline constexpr int32_t NICE_RANGE = NICE_MAX - NICE_MIN; // 39

// =============================================================================
// Helper Functions
// =============================================================================

/// Pixel geometry of the priority slider at one font size.
struct PrioritySliderMetrics
{
    float sliderWidth = 0.0F;
    float sliderHeight = 0.0F;
    float badgeHeight = 0.0F;
    float badgeArrowSize = 0.0F;
    float sliderCornerRadius = 0.0F;
    float badgeCornerRadius = 0.0F;
    float thumbRadius = 0.0F;
    float thumbOutlineThickness = 0.0F;
    float labelPadding = 0.0F;
};

/**
 * @brief Resolve the slider's em geometry to pixels for the current font
 *
 * The track is the only part that can outgrow its container: at Even Huger on a scaled display its
 * authored width is well over a thousand pixels, so it is capped to the space the panel actually
 * has. The floor deliberately wins over that cap -- a track that overflows a very narrow panel is
 * still usable by scrolling, one squeezed to nothing is not.
 *
 * @param emPx One em, i.e. ImGui::GetFontSize()
 * @param availableTrackWidthPx Width left for the track once the High/Low labels and their padding
 *        are accounted for; non-positive or non-finite means "unconstrained"
 * @return PrioritySliderMetrics Geometry in pixels
 */
[[nodiscard]] inline auto computePrioritySliderMetrics(float emPx, float availableTrackWidthPx) noexcept -> PrioritySliderMetrics
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;

    float sliderWidth = PRIORITY_SLIDER_WIDTH_EM * em;
    if (std::isfinite(availableTrackWidthPx) && availableTrackWidthPx > 0.0F)
    {
        sliderWidth = std::max(std::min(sliderWidth, availableTrackWidthPx), PRIORITY_SLIDER_MIN_WIDTH_EM * em);
    }

    const float sliderHeight = PRIORITY_SLIDER_HEIGHT_EM * em;

    return PrioritySliderMetrics{
        .sliderWidth = sliderWidth,
        .sliderHeight = sliderHeight,
        .badgeHeight = PRIORITY_BADGE_HEIGHT_EM * em,
        .badgeArrowSize = PRIORITY_BADGE_ARROW_SIZE_EM * em,
        .sliderCornerRadius = PRIORITY_SLIDER_CORNER_RADIUS_EM * em,
        .badgeCornerRadius = PRIORITY_BADGE_CORNER_RADIUS_EM * em,
        .thumbRadius = sliderHeight * PRIORITY_THUMB_RADIUS_FRACTION,
        .thumbOutlineThickness = std::max(PRIORITY_THUMB_OUTLINE_THICKNESS_EM * em, PRIORITY_THUMB_OUTLINE_MIN_PX),
        .labelPadding = PRIORITY_LABEL_PADDING_EM * em,
    };
}

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
