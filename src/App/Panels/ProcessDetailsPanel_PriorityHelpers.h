#pragma once

// This header exposes priority slider helper functions for testing and
// internal use. These were extracted from ProcessDetailsPanel::renderActions()
// to improve testability and code organization.

#include "Domain/PriorityConfig.h"
#include "UI/ColorContrast.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

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
 * has. There is deliberately no minimum width that could win over that cap: the panel's content
 * area does not scroll horizontally, so a track wider than the space available is clipped and the
 * nice values past the clip cannot be reached with the pointer. A short track that is all on screen
 * is still fully usable, by pointer and by keyboard.
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
        sliderWidth = std::min(sliderWidth, availableTrackWidthPx);
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
 * @brief Horizontal centre of the value badge, kept over the track
 *
 * The badge follows the thumb but is not allowed to hang off either end of the track. When the track
 * is narrower than the badge there is no position that satisfies both ends, so the badge is centred
 * on the track instead -- std::clamp with its bounds crossed is undefined behaviour.
 *
 * @param thumbX Screen X of the thumb, which the badge wants to sit above
 * @param trackStartX Screen X of the track's left edge
 * @param trackWidthPx Width of the track
 * @param badgeHalfWidthPx Half the badge's width
 * @return float Screen X for the badge's centre
 */
[[nodiscard]] inline auto computeBadgeCenterX(float thumbX, float trackStartX, float trackWidthPx, float badgeHalfWidthPx) noexcept -> float
{
    const float lowest = trackStartX + badgeHalfWidthPx;
    const float highest = trackStartX + trackWidthPx - badgeHalfWidthPx;
    if (!(lowest <= highest))
    {
        return trackStartX + (trackWidthPx * 0.5F);
    }
    return std::clamp(thumbX, lowest, highest);
}

/// The x range of the badge's down-arrow base: centred under @p tipX, @p arrowHalfBasePx either side,
/// but held to the badge's flat bottom edge [@p badgeMinX, @p badgeMaxX], so the arrow never hangs off
/// the badge where it is clamped at a track end (#1533). A badge narrower than the arrow gives the
/// whole badge width.
struct BadgeArrowBase
{
    float left = 0.0F;
    float right = 0.0F;
};

[[nodiscard]] inline auto computeBadgeArrowBase(float tipX, float arrowHalfBasePx, float badgeMinX, float badgeMaxX) noexcept
    -> BadgeArrowBase
{
    if (!(badgeMinX <= badgeMaxX))
    {
        return {.left = tipX, .right = tipX};
    }
    return {.left = std::clamp(tipX - arrowHalfBasePx, badgeMinX, badgeMaxX),
            .right = std::clamp(tipX + arrowHalfBasePx, badgeMinX, badgeMaxX)};
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

/// WCAG 2 floor for the nice value drawn on the badge: small text (#1130).
inline constexpr float PRIORITY_BADGE_TEXT_MIN_CONTRAST = 4.5F;

/**
 * @brief Unpack a colour packed by getNiceColor() into floats
 *
 * Header-only (no ImGui runtime), so the pure contrast helpers below can be fed what the panel draws.
 */
[[nodiscard]] constexpr auto unpackColor(ImU32 packed) noexcept -> ImVec4
{
    constexpr float SCALE = 1.0F / 255.0F;
    return {static_cast<float>((packed >> IM_COL32_R_SHIFT) & 0xFFU) * SCALE,
            static_cast<float>((packed >> IM_COL32_G_SHIFT) & 0xFFU) * SCALE,
            static_cast<float>((packed >> IM_COL32_B_SHIFT) & 0xFFU) * SCALE,
            static_cast<float>((packed >> IM_COL32_A_SHIFT) & 0xFFU) * SCALE};
}

/**
 * @brief The colour to draw on a priority badge (and the slider thumb) of colour `fill` (#1130)
 *
 * The nice value was drawn in the theme's fixed priority.badge_text over whatever getNiceColor()
 * produced, so on most dark themes white text sat on a light green badge at nice 0 (Arctic Fire 1.67:1).
 * The theme's two poles, checked against the WCAG small-text floor in order: its badge text colour
 * when that is readable on this fill, otherwise its window background when that is. If neither
 * reaches the floor, pure black or white -- whichever contrasts more -- always does (at least
 * 4.58:1). Unlike readableTextOn()'s "clearly better" margin, a readable preferred colour is kept
 * however much better the alternate would be.
 *
 * @param fill The badge's fill, i.e. getNiceColor() at the nice value shown
 * @param badgeText The theme's priority.badge_text, kept when it is readable
 * @param windowBg The theme's window background, the other pole of its palette
 * @return ImVec4 An opaque text colour with at least PRIORITY_BADGE_TEXT_MIN_CONTRAST on `fill`
 */
[[nodiscard]] inline auto badgeTextFor(const ImVec4& fill, const ImVec4& badgeText, const ImVec4& windowBg) noexcept -> ImVec4
{
    for (ImVec4 pole : {badgeText, windowBg})
    {
        pole.w = 1.0F;
        if (UI::ColorContrast::contrastRatio(pole, fill) >= PRIORITY_BADGE_TEXT_MIN_CONTRAST)
        {
            return pole;
        }
    }
    const ImVec4 black{0.0F, 0.0F, 0.0F, 1.0F};
    const ImVec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    return (UI::ColorContrast::contrastRatio(black, fill) >= UI::ColorContrast::contrastRatio(white, fill)) ? black : white;
}

// =============================================================================
// Windows priority classes (#1204)
// =============================================================================
//
// Windows has six priority classes, not forty nice steps, so on Windows Process Details offers the
// classes by name, as stops on the discrete slider (#1538). The probe reports each class as a nice value in the middle of its
// getPriorityLabel() bucket (Platform::priorityClassToNice) and setPriority() maps a nice value back
// to a class (Platform::niceToPriorityClass), so the control writes one representative nice value
// per class through the same setPriority() path. Pure and platform-independent, so it is tested on
// every platform; the panel uses it only under _WIN32.

/// Whether this build shows Windows priority classes rather than Unix nice values.
#ifdef _WIN32
inline constexpr bool PRIORITY_USES_WINDOWS_CLASSES = true;
#else
inline constexpr bool PRIORITY_USES_WINDOWS_CLASSES = false;
#endif

/// A Windows priority class, lowest first: Domain's, which the Processes table names too (#1280).
/// Realtime is reported, never set: Platform::niceToPriorityClass() deliberately stops at High.
/// None never comes from windowsPriorityClassFromNice().
using WindowsPriorityClass = Domain::Priority::PriorityClass;

/// The classes the priority control offers, in its order. Realtime is not among them.
inline constexpr std::array<WindowsPriorityClass, 5> SETTABLE_WINDOWS_PRIORITY_CLASSES = {
    WindowsPriorityClass::Idle,
    WindowsPriorityClass::BelowNormal,
    WindowsPriorityClass::Normal,
    WindowsPriorityClass::AboveNormal,
    WindowsPriorityClass::High,
};

/// The nice value that stands for @p priorityClass: what the probe reports for it and what the
/// control passes to setPriority() to select it. Mirrors Platform::priorityClassToNice(), which this
/// layer cannot include (it pulls in <windows.h>); the tests hold both to getPriorityLabel().
[[nodiscard]] constexpr auto windowsPriorityClassNice(WindowsPriorityClass priorityClass) noexcept -> int32_t
{
    switch (priorityClass)
    {
    case WindowsPriorityClass::Idle:
        return Domain::Priority::MAX_NICE;
    case WindowsPriorityClass::BelowNormal:
        return 10;
    case WindowsPriorityClass::AboveNormal:
        return -7;
    case WindowsPriorityClass::High:
        return -15;
    case WindowsPriorityClass::Realtime:
        return Domain::Priority::MIN_NICE;
    case WindowsPriorityClass::Normal:
    default:
        return Domain::Priority::NORMAL_NICE;
    }
}

/// The class a reported nice value stands for. Realtime is MIN_NICE, which nothing else reports on
/// Windows; every other value falls in a getPriorityLabel() bucket.
[[nodiscard]] constexpr auto windowsPriorityClassFromNice(int32_t nice) noexcept -> WindowsPriorityClass
{
    if (nice <= Domain::Priority::MIN_NICE)
    {
        return WindowsPriorityClass::Realtime;
    }
    if (nice < Domain::Priority::HIGH_THRESHOLD)
    {
        return WindowsPriorityClass::High;
    }
    if (nice < Domain::Priority::ABOVE_NORMAL_THRESHOLD)
    {
        return WindowsPriorityClass::AboveNormal;
    }
    if (nice < Domain::Priority::BELOW_NORMAL_THRESHOLD)
    {
        return WindowsPriorityClass::Normal;
    }
    if (nice < Domain::Priority::IDLE_THRESHOLD)
    {
        return WindowsPriorityClass::BelowNormal;
    }
    return WindowsPriorityClass::Idle;
}

/// The class's name, as the Processes table's Priority column spells it
/// (Domain::Priority::getPriorityClassLabel(), #1280).
[[nodiscard]] constexpr auto windowsPriorityClassName(WindowsPriorityClass priorityClass) noexcept -> std::string_view
{
    return Domain::Priority::getPriorityClassLabel(priorityClass);
}

/// Process Details' Overview priority text: the class name on Windows, where nice values mean
/// nothing to the user (#1204); the label with its nice value elsewhere.
[[nodiscard]] inline auto priorityDisplayText(int32_t nice, bool windowsClasses) -> std::string
{
    if (windowsClasses)
    {
        return std::string(windowsPriorityClassName(windowsPriorityClassFromNice(nice)));
    }
    return std::format("{} (nice: {})", Domain::Priority::getPriorityLabel(nice), nice);
}

// =============================================================================
// Discrete-stop slider (#1538)
// =============================================================================
//
// The priority slider's discrete mode, for a priority that is a handful of named classes rather than
// forty nice steps: Windows' priority classes, and Linux's I/O priority classes (#1540). N evenly
// spaced stops with snapping, a tick under each and the stop's name on the badge. Stop 0 is at the
// left, the high-priority end, where nice -20 is on the continuous slider. Each stop carries a nice
// value only for its colour, so getNiceColor() and the gradient are shared with that slider.
// Detail::renderDiscretePrioritySlider() (ProcessPriorityView.h) draws it.

/// Room left of the track for a state shown beyond the high end (Windows' Realtime), in ems.
inline constexpr float PRIORITY_DISCRETE_LEAD_EM = 1.75F;

/// The shown index of a state beyond the high end: shown, never picked.
inline constexpr int32_t PRIORITY_STOP_BEYOND_START = -1;

/// One stop of a discrete slider.
struct PrioritySliderStop
{
    std::string_view name;      ///< The badge's text and the stop's scale label ("Below Normal")
    const char* itemLabel = ""; ///< The control's ImGui label at this stop, naming it for assistive tools;
                                ///< it ends in the slider's "###" ID, so the ID holds as the value moves
    int32_t colorNice = 0;      ///< The nice value whose getNiceColor() colours this stop
};

/// A discrete slider: its stops, left (highest priority) to right, at least one, and optionally a
/// state beyond the high end that can be shown but not picked.
struct DiscretePrioritySlider
{
    std::span<const PrioritySliderStop> stops;
    const PrioritySliderStop* beyondStart = nullptr;
    const char* tooltip = ""; ///< Shown while the track is hovered
};

/// Where stop @p index of @p count sits on the track, 0 (left) to 1 (right); a lone stop sits at 0.
[[nodiscard]] constexpr auto discreteStopPosition(int32_t index, int32_t count) noexcept -> float
{
    if (count <= 1)
    {
        return 0.0F;
    }
    return static_cast<float>(std::clamp(index, 0, count - 1)) / static_cast<float>(count - 1);
}

/// The stop nearest @p position (0 to 1, held there): where a drag snaps.
[[nodiscard]] inline auto discreteStopFromPosition(float position, int32_t count) noexcept -> int32_t
{
    if (count <= 1 || !std::isfinite(position))
    {
        return 0;
    }
    return static_cast<int32_t>(std::lround(std::clamp(position, 0.0F, 1.0F) * static_cast<float>(count - 1)));
}

/// @p current moved @p delta stops (negative is toward the high end), held to the stops: the arrow,
/// Home and End keys. From beyond the start (PRIORITY_STOP_BEYOND_START, shown but never picked) only a
/// move toward the low end leaves, landing on the stop it reaches; a move toward the high end stays.
[[nodiscard]] constexpr auto stepDiscreteStop(int32_t current, int32_t delta, int32_t count) noexcept -> int32_t
{
    if (count <= 0)
    {
        return current;
    }
    if (current < 0)
    {
        return (delta > 0) ? std::min(delta - 1, count - 1) : current;
    }
    return std::clamp(current + delta, 0, count - 1);
}

/// The nice value whose colour the track has at @p position (0 to 1): the stops' colour values,
/// interpolated between neighbours, so each tick sits on its own stop's colour.
[[nodiscard]] inline auto discreteTrackColorNice(float position, std::span<const PrioritySliderStop> stops) noexcept -> int32_t
{
    if (stops.empty())
    {
        return Domain::Priority::NORMAL_NICE;
    }
    if (stops.size() == 1 || !std::isfinite(position))
    {
        return stops.front().colorNice;
    }
    const float scaled = std::clamp(position, 0.0F, 1.0F) * static_cast<float>(stops.size() - 1);
    const std::size_t lower = std::min(static_cast<std::size_t>(scaled), stops.size() - 2);
    const float t = scaled - static_cast<float>(lower);
    const int32_t from = stops[lower].colorNice;
    return from + static_cast<int32_t>(std::lround(t * static_cast<float>(stops[lower + 1].colorNice - from)));
}

/// The left edge of stop @p index's scale label, @p labelWidthPx wide, whose stop is at @p stopX: the
/// first label starts at its stop and the last ends at its stop, so neither runs past the track's
/// ends; the others are centred on theirs.
[[nodiscard]] constexpr auto discreteStopLabelX(float stopX, float labelWidthPx, int32_t index, int32_t count) noexcept -> float
{
    if (index <= 0)
    {
        return stopX;
    }
    if (index >= count - 1)
    {
        return stopX - labelWidthPx;
    }
    return stopX - (labelWidthPx * 0.5F);
}

/// The narrowest track on which @p count scale labels, stop i's @p labelWidthPx(i) wide, placed by
/// discreteStopLabelX(), stay @p gapPx apart: the stops are evenly spaced, so the closest pair sets it.
template<typename LabelWidth>
[[nodiscard]] auto discreteStopLabelsMinWidth(std::size_t count, const LabelWidth& labelWidthPx, float gapPx) -> float
{
    if (count <= 1)
    {
        return count == 0 ? 0.0F : labelWidthPx(0);
    }
    float widestSpacing = 0.0F;
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        const float rightOfStop = (i == 0) ? labelWidthPx(i) : labelWidthPx(i) * 0.5F;
        const float leftOfNext = (i + 2 == count) ? labelWidthPx(i + 1) : labelWidthPx(i + 1) * 0.5F;
        widestSpacing = std::max(widestSpacing, rightOfStop + leftOfNext + gapPx);
    }
    return widestSpacing * static_cast<float>(count - 1);
}

// Windows' priority classes on the discrete slider (#1538): the five settable classes, highest
// first, so High is at the left like nice -20; Realtime is shown beyond High, never picked.

/// The settable classes in the slider's order: SETTABLE_WINDOWS_PRIORITY_CLASSES, highest first.
inline constexpr std::array<WindowsPriorityClass, 5> WINDOWS_PRIORITY_SLIDER_ORDER = {
    WindowsPriorityClass::High,
    WindowsPriorityClass::AboveNormal,
    WindowsPriorityClass::Normal,
    WindowsPriorityClass::BelowNormal,
    WindowsPriorityClass::Idle,
};

/// Normal's stop: the middle one.
inline constexpr int32_t WINDOWS_NORMAL_STOP = 2;
static_assert(WINDOWS_PRIORITY_SLIDER_ORDER[static_cast<std::size_t>(WINDOWS_NORMAL_STOP)] == WindowsPriorityClass::Normal);

/// The slider stop for @p priorityClass: PRIORITY_STOP_BEYOND_START for Realtime (shown, never set),
/// Normal's stop for None (which windowsPriorityClassFromNice() never gives).
[[nodiscard]] constexpr auto windowsPriorityStopIndex(WindowsPriorityClass priorityClass) noexcept -> int32_t
{
    if (priorityClass == WindowsPriorityClass::Realtime)
    {
        return PRIORITY_STOP_BEYOND_START;
    }
    for (std::size_t i = 0; i < WINDOWS_PRIORITY_SLIDER_ORDER.size(); ++i)
    {
        if (WINDOWS_PRIORITY_SLIDER_ORDER[i] == priorityClass)
        {
            return static_cast<int32_t>(i);
        }
    }
    return WINDOWS_NORMAL_STOP;
}

/// The class at slider stop @p index (held to the stops); Realtime beyond the start.
[[nodiscard]] constexpr auto windowsPriorityClassAtStop(int32_t index) noexcept -> WindowsPriorityClass
{
    if (index < 0)
    {
        return WindowsPriorityClass::Realtime;
    }
    const auto last = static_cast<int32_t>(WINDOWS_PRIORITY_SLIDER_ORDER.size()) - 1;
    return WINDOWS_PRIORITY_SLIDER_ORDER[static_cast<std::size_t>(std::min(index, last))];
}

/// Windows' stops: the class name, a label naming it, and its representative nice value's colour.
inline constexpr std::array<PrioritySliderStop, 5> WINDOWS_PRIORITY_STOPS = {{
    {.name = windowsPriorityClassName(WindowsPriorityClass::High),
     .itemLabel = "Priority class: High###priority_class_slider",
     .colorNice = windowsPriorityClassNice(WindowsPriorityClass::High)},
    {.name = windowsPriorityClassName(WindowsPriorityClass::AboveNormal),
     .itemLabel = "Priority class: Above Normal###priority_class_slider",
     .colorNice = windowsPriorityClassNice(WindowsPriorityClass::AboveNormal)},
    {.name = windowsPriorityClassName(WindowsPriorityClass::Normal),
     .itemLabel = "Priority class: Normal###priority_class_slider",
     .colorNice = windowsPriorityClassNice(WindowsPriorityClass::Normal)},
    {.name = windowsPriorityClassName(WindowsPriorityClass::BelowNormal),
     .itemLabel = "Priority class: Below Normal###priority_class_slider",
     .colorNice = windowsPriorityClassNice(WindowsPriorityClass::BelowNormal)},
    {.name = windowsPriorityClassName(WindowsPriorityClass::Idle),
     .itemLabel = "Priority class: Idle###priority_class_slider",
     .colorNice = windowsPriorityClassNice(WindowsPriorityClass::Idle)},
}};

/// Realtime, shown beyond High when a process already has it.
inline constexpr PrioritySliderStop WINDOWS_REALTIME_STOP{
    .name = windowsPriorityClassName(WindowsPriorityClass::Realtime),
    .itemLabel = "Priority class: Realtime (set outside TaskSmack)###priority_class_slider",
    .colorNice = windowsPriorityClassNice(WindowsPriorityClass::Realtime),
};

/// The ImGui ID every Windows stop's itemLabel ends in.
inline constexpr const char* WINDOWS_PRIORITY_SLIDER_ID = "###priority_class_slider";

/// The Windows priority slider.
inline constexpr DiscretePrioritySlider WINDOWS_PRIORITY_SLIDER{
    .stops = WINDOWS_PRIORITY_STOPS,
    .beyondStart = &WINDOWS_REALTIME_STOP,
    .tooltip = "Windows priority class: higher classes get CPU time first.\n"
               "Realtime cannot be set here.\n\n"
               "Keyboard shortcuts:\n"
               "  Left/Right, Up/Down: One class higher or lower\n"
               "  Home/End: Highest/lowest class\n\n"
               "Note: Changing another user's or an elevated process typically requires administrator privileges",
};

/// The nice value to pass setPriority() for slider stop @p picked when @p shown is the value on show:
/// @p shown itself while the stop is still its class's (so an untouched control is no edit), else the
/// picked class's representative value.
[[nodiscard]] constexpr auto windowsPriorityNiceForStop(int32_t picked, int32_t shown) noexcept -> int32_t
{
    if (picked == windowsPriorityStopIndex(windowsPriorityClassFromNice(shown)))
    {
        return shown;
    }
    return windowsPriorityClassNice(windowsPriorityClassAtStop(picked));
}

// Note: For priority labels, use Domain::Priority::getPriorityLabel() from PriorityConfig.h
// to maintain consistency across the application.

} // namespace App::Detail
