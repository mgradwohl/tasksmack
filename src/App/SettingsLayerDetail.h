#pragma once

/// @file SettingsLayerDetail.h
/// @brief Internal implementation details for SettingsLayer, exposed for testing.
/// @note These functions and types are not part of the public API and may change.

#include "UI/FontPointSize.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace App::Detail
{

// ========================================
// Font size options: every whole point size (#3375)
// ========================================
struct FontSizeOption
{
    std::string_view label;
    int points;
};

inline constexpr std::array<FontSizeOption, 26> FONT_SIZE_OPTIONS = {
    {
        {.label = "7 pt", .points = 7},   {.label = "8 pt", .points = 8},   {.label = "9 pt", .points = 9},
        {.label = "10 pt", .points = 10}, {.label = "11 pt", .points = 11}, {.label = "12 pt", .points = 12},
        {.label = "13 pt", .points = 13}, {.label = "14 pt", .points = 14}, {.label = "15 pt", .points = 15},
        {.label = "16 pt", .points = 16}, {.label = "17 pt", .points = 17}, {.label = "18 pt", .points = 18},
        {.label = "19 pt", .points = 19}, {.label = "20 pt", .points = 20}, {.label = "21 pt", .points = 21},
        {.label = "22 pt", .points = 22}, {.label = "23 pt", .points = 23}, {.label = "24 pt", .points = 24},
        {.label = "25 pt", .points = 25}, {.label = "26 pt", .points = 26}, {.label = "27 pt", .points = 27},
        {.label = "28 pt", .points = 28}, {.label = "29 pt", .points = 29}, {.label = "30 pt", .points = 30},
        {.label = "31 pt", .points = 31}, {.label = "32 pt", .points = 32},
    },
};
static_assert(FONT_SIZE_OPTIONS.front().points == UI::FONT_POINT_SIZE_MIN && FONT_SIZE_OPTIONS.back().points == UI::FONT_POINT_SIZE_MAX &&
                  FONT_SIZE_OPTIONS.size() ==
                      static_cast<std::size_t>(UI::FONT_POINT_SIZE_MAX) - static_cast<std::size_t>(UI::FONT_POINT_SIZE_MIN) + 1,
              "one Font size option per point size in the setting's range");

// ========================================
// Interface scale options (percent; 0 = Auto) (#3376)
// ========================================
struct InterfaceScaleOption
{
    std::string_view label;
    int percent; ///< 0: Auto, the display's own scale
};

inline constexpr std::array<InterfaceScaleOption, 9> INTERFACE_SCALE_OPTIONS = {
    {
        {.label = "Auto", .percent = 0},
        {.label = "100%", .percent = 100},
        {.label = "125%", .percent = 125},
        {.label = "150%", .percent = 150},
        {.label = "175%", .percent = 175},
        {.label = "200%", .percent = 200},
        {.label = "225%", .percent = 225},
        {.label = "250%", .percent = 250},
        {.label = "300%", .percent = 300},
    },
};

/// The Interface scale combo's "Auto" text: Auto and the scale the display reports, as a whole percent
/// ("Auto (175%)"), so Auto never hides what it picked. Just "Auto" when the report isn't a usable scale.
[[nodiscard]] inline std::string interfaceScaleAutoLabel(float measuredScale)
{
    if (!(measuredScale > 0.0F) || !std::isfinite(measuredScale))
    {
        return "Auto";
    }
    return std::format("Auto ({}%)", static_cast<int>(std::lround(measuredScale * 100.0F)));
}

// ========================================
// Refresh rate options (milliseconds)
// ========================================
struct RefreshRateOption
{
    std::string_view label;
    int valueMs;
};

inline constexpr std::array<RefreshRateOption, 6> REFRESH_RATE_OPTIONS = {
    {
        {.label = "100 ms", .valueMs = 100},
        {.label = "250 ms", .valueMs = 250},
        {.label = "500 ms", .valueMs = 500},
        {.label = "1 second", .valueMs = 1000},
        {.label = "2 seconds", .valueMs = 2000},
        {.label = "5 seconds", .valueMs = 5000},
    },
};

// ========================================
// History duration options (seconds)
// ========================================
struct HistoryOption
{
    std::string_view label;
    int valueSeconds;
};

inline constexpr std::array<HistoryOption, 4> HISTORY_OPTIONS = {
    {
        {.label = "1 minute", .valueSeconds = 60},
        {.label = "2 minutes", .valueSeconds = 120},
        {.label = "5 minutes", .valueSeconds = 300},
        {.label = "10 minutes", .valueSeconds = 600},
    },
};

// ========================================
// Combo state and what Save writes (#1120, #1151)
// ========================================

/// One Settings combo: the option matching the stored value when the dialog opened (nullopt when
/// the stored value is none of the options, e.g. interval_ms = 750 in config.toml), and whether the
/// user has picked an option since.
struct ComboState
{
    std::optional<std::size_t> index;
    bool touched = false;
};

/// What Save should write for a combo: the option the user picked, or nothing for a control left
/// untouched. Writing every control back used to replace stored values the dialog can't show with
/// a fallback option (#1120), and to undo a font size changed by Ctrl+= while the dialog was open
/// (#1151).
template<typename Option, std::size_t N>
[[nodiscard]] std::optional<Option> pickedOption(const ComboState& state, const std::array<Option, N>& options)
{
    if (!state.touched || !state.index.has_value() || *state.index >= N)
    {
        return std::nullopt;
    }
    return options[*state.index];
}

/// Index of the option whose value equals `value`, if there is one.
template<typename Option, std::size_t N, typename Value, typename Projection>
[[nodiscard]] std::optional<std::size_t> optionIndexOf(const std::array<Option, N>& options, const Value& value, Projection projection)
{
    // NOLINTNEXTLINE(readability-qualified-auto) - iterator type varies by platform
    const auto it = std::ranges::find(options, value, projection);
    if (it == options.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - options.begin());
}

/// A combo set back to its default by Reset to defaults: on the option whose value is
/// `defaultValue`, and picked, so Save writes it. A default that is none of the options (which no
/// shipped default is) leaves the combo untouched rather than writing a fallback option.
template<typename Option, std::size_t N, typename Value, typename Projection>
[[nodiscard]] ComboState defaultChoice(const std::array<Option, N>& options, const Value& defaultValue, Projection projection)
{
    const std::optional<std::size_t> index = optionIndexOf(options, defaultValue, projection);
    return ComboState{.index = index, .touched = index.has_value()};
}

/// Combo preview for a refresh interval that isn't one of the options.
[[nodiscard]] inline std::string customRefreshLabel(int ms)
{
    return std::format("Custom ({} ms)", ms);
}

/// Combo preview for a history window that isn't one of the options.
[[nodiscard]] inline std::string customHistoryLabel(int seconds)
{
    if (seconds % 60 == 0)
    {
        const int minutes = seconds / 60;
        return std::format("Custom ({} minute{})", minutes, minutes == 1 ? "" : "s");
    }
    return std::format("Custom ({} seconds)", seconds);
}

} // namespace App::Detail
