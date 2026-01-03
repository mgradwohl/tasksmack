#pragma once

/// @file SettingsLayerDetail.h
/// @brief Internal implementation details for SettingsLayer, exposed for testing.
/// @note These functions and types are not part of the public API and may change.

#include "UI/Theme.h"

#include <array>
#include <cstddef>

namespace App::detail
{

// ========================================
// Font size options
// ========================================
struct FontSizeOption
{
    const char* label;
    UI::FontSize value;
};

inline constexpr std::array<FontSizeOption, 6> FONT_SIZE_OPTIONS = {{
    {.label = "Small", .value = UI::FontSize::Small},
    {.label = "Medium", .value = UI::FontSize::Medium},
    {.label = "Large", .value = UI::FontSize::Large},
    {.label = "Extra Large", .value = UI::FontSize::ExtraLarge},
    {.label = "Huge", .value = UI::FontSize::Huge},
    {.label = "Even Huger", .value = UI::FontSize::EvenHuger},
}};

// ========================================
// Refresh rate options (milliseconds)
// ========================================
struct RefreshRateOption
{
    const char* label;
    int valueMs;
};

inline constexpr std::array<RefreshRateOption, 6> REFRESH_RATE_OPTIONS = {{
    {.label = "100 ms", .valueMs = 100},
    {.label = "250 ms", .valueMs = 250},
    {.label = "500 ms", .valueMs = 500},
    {.label = "1 second", .valueMs = 1000},
    {.label = "2 seconds", .valueMs = 2000},
    {.label = "5 seconds", .valueMs = 5000},
}};

// ========================================
// History duration options (seconds)
// ========================================
struct HistoryOption
{
    const char* label;
    int valueSeconds;
};

inline constexpr std::array<HistoryOption, 4> HISTORY_OPTIONS = {{
    {.label = "1 minute", .valueSeconds = 60},
    {.label = "2 minutes", .valueSeconds = 120},
    {.label = "5 minutes", .valueSeconds = 300},
    {.label = "10 minutes", .valueSeconds = 600},
}};

// ========================================
// Index lookup functions
// Return the index of the matching value, or a sensible default if not found.
// ========================================

/// Find the index for a given font size.
/// @return Index into FONT_SIZE_OPTIONS, or 1 (Medium) if not found.
[[nodiscard]] inline auto findFontSizeIndex(UI::FontSize size) -> std::size_t
{
    for (std::size_t i = 0; i < FONT_SIZE_OPTIONS.size(); ++i)
    {
        if (FONT_SIZE_OPTIONS[i].value == size)
        {
            return i;
        }
    }
    return 1; // Default to Medium
}

/// Find the index for a given refresh rate in milliseconds.
/// @return Index into REFRESH_RATE_OPTIONS, or 3 (1 second) if not found.
[[nodiscard]] inline auto findRefreshRateIndex(int ms) -> std::size_t
{
    for (std::size_t i = 0; i < REFRESH_RATE_OPTIONS.size(); ++i)
    {
        if (REFRESH_RATE_OPTIONS[i].valueMs == ms)
        {
            return i;
        }
    }
    return 3; // Default to 1 second
}

/// Find the index for a given history duration in seconds.
/// @return Index into HISTORY_OPTIONS, or 2 (5 minutes) if not found.
[[nodiscard]] inline auto findHistoryIndex(int seconds) -> std::size_t
{
    for (std::size_t i = 0; i < HISTORY_OPTIONS.size(); ++i)
    {
        if (HISTORY_OPTIONS[i].valueSeconds == seconds)
        {
            return i;
        }
    }
    return 2; // Default to 5 minutes
}

} // namespace App::detail
