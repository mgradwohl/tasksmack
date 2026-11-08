#pragma once

/// @file FontPointSize.h
/// @brief The Font size setting as a point size (#3375): its range, its default, the heading and chart sizes that
/// follow from it, and the named sizes it replaced. No SDL or ImGui, so App's config code (and the user-config
/// fuzzer, which builds without them) can use it; Theme.h includes it.

#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace UI
{

/// The Font size setting's range, in whole points (#3375). The floor keeps body text at 9 px or more at 100% scale,
/// as the smallest of the old named sizes did (#1194): 7 pt is about 9.3 px. Fonts are rasterized at pt * 96 / 72 px
/// at 100% (computePointsToPixels(), DpiScale.h), so a point size is the same size on Windows and Linux at the same
/// display or Interface scale (#3376).
inline constexpr int FONT_POINT_SIZE_MIN = 7;
inline constexpr int FONT_POINT_SIZE_MAX = 32;

/// The default body size: 8 pt, the old default "Medium" size exactly, so a first run and an existing config both
/// look as they did before point sizes.
inline constexpr int FONT_POINT_SIZE_DEFAULT = 8;

/// Headings are this many points larger than the body text, as every old named size had them (7/9 to 16/18).
inline constexpr int FONT_HEADING_POINT_STEP = 2;

/// Chart axis labels, legends and hints are this many points smaller than the body text, but never below
/// CHART_FONT_POINT_FLOOR, and never larger than the body text (#1194).
inline constexpr int CHART_FONT_POINT_STEP = 2;
inline constexpr int CHART_FONT_POINT_FLOOR = 8;

/// @p points clamped to [FONT_POINT_SIZE_MIN, FONT_POINT_SIZE_MAX].
[[nodiscard]] constexpr int clampFontPointSize(int points) noexcept
{
    if (points < FONT_POINT_SIZE_MIN)
    {
        return FONT_POINT_SIZE_MIN;
    }
    if (points > FONT_POINT_SIZE_MAX)
    {
        return FONT_POINT_SIZE_MAX;
    }
    return points;
}

/// A point size read from config.toml, which may be fractional or out of range (a hand edit): rounded to a whole
/// point and clamped. Anything that isn't a number (NaN, infinity) is the default.
[[nodiscard]] inline int fontPointSizeFromConfig(double points) noexcept
{
    if (!std::isfinite(points))
    {
        return FONT_POINT_SIZE_DEFAULT;
    }
    if (points <= static_cast<double>(FONT_POINT_SIZE_MIN))
    {
        return FONT_POINT_SIZE_MIN;
    }
    if (points >= static_cast<double>(FONT_POINT_SIZE_MAX))
    {
        return FONT_POINT_SIZE_MAX;
    }
    return clampFontPointSize(static_cast<int>(std::lround(points)));
}

/// The three sizes the fonts are built at for one body size, in points.
struct FontPointSizes
{
    float regularPt = 0.0F; ///< Body text
    float largePt = 0.0F;   ///< Headings
    float chartPt = 0.0F;   ///< Chart axis labels, legends and hints
};

/// The chart text size for body text of @p bodyPoints: two points smaller, but at least CHART_FONT_POINT_FLOOR and at
/// most the body size. At the old named sizes it is exactly what each one used (Small's 7 pt kept 7, Medium and
/// Large drew charts at 8, Extra Large at 10, Huge at 12, Largest at 14).
[[nodiscard]] constexpr int chartFontPointSize(int bodyPoints) noexcept
{
    const int body = clampFontPointSize(bodyPoints);
    const int stepped = body - CHART_FONT_POINT_STEP;
    const int floored = stepped < CHART_FONT_POINT_FLOOR ? CHART_FONT_POINT_FLOOR : stepped;
    return floored > body ? body : floored;
}

/// The body, heading and chart sizes for the Font size setting @p bodyPoints (clamped to the range first).
[[nodiscard]] constexpr FontPointSizes fontPointSizesFor(int bodyPoints) noexcept
{
    const int body = clampFontPointSize(bodyPoints);
    return {
        .regularPt = static_cast<float>(body),
        .largePt = static_cast<float>(body + FONT_HEADING_POINT_STEP),
        .chartPt = static_cast<float>(chartFontPointSize(body)),
    };
}

/// Chart text size as a fraction of body text size for body text of @p bodyPoints: 1.0 at 7 and 8 pt, where charts
/// draw at the body size (chartFontPointSize()), and below 1.0 above them.
[[nodiscard]] constexpr float chartFontScale(int bodyPoints) noexcept
{
    const FontPointSizes sizes = fontPointSizesFor(bodyPoints);
    return sizes.chartPt / sizes.regularPt;
}

/// A named size the Font size setting used to be (config.toml's `[font] size`), and the point size it was.
struct LegacyFontSize
{
    std::string_view configName;
    int points = 0;
};

/// The named sizes before #3375, as config.toml spelled them, with their body sizes. "even-huger" is the one Settings
/// called "Largest".
inline constexpr auto LEGACY_FONT_SIZES = std::to_array<LegacyFontSize>({
    {.configName = "small", .points = 7},
    {.configName = "medium", .points = 8},
    {.configName = "large", .points = 10},
    {.configName = "extra-large", .points = 12},
    {.configName = "huge", .points = 14},
    {.configName = "even-huger", .points = 16},
});

/// The point size an old config.toml's `[font] size = "<name>"` meant, or nothing for a name that was never one.
[[nodiscard]] constexpr std::optional<int> legacyFontSizePoints(std::string_view configName) noexcept
{
    for (const LegacyFontSize& legacy : LEGACY_FONT_SIZES)
    {
        if (legacy.configName == configName)
        {
            return legacy.points;
        }
    }
    return std::nullopt;
}

} // namespace UI
