#pragma once

// What the status bar says, and how much of it fits (#1200). Pure and ImGui-free, so the wording and
// the narrow-window fitting are unit-testable on their own (CONTRIBUTING.md's "extract the pure
// decision logic into a small header" pattern).
//
// The status bar used to say "Ready" for the whole session, beside an FPS readout that was always
// shown. It now says what TaskSmack is doing -- how many processes it is tracking and how often it
// updates -- and the frame rate only while Render Metrics (Ctrl+Shift+M) is on, since it is a
// diagnostic rather than something a user monitors.

#include "UI/Format.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace App::StatusBarText
{

/// Between two status segments.
inline constexpr const char* SEPARATOR = "  \xC2\xB7  "; // a middle dot, spaced

/// "312 processes"; "1 process". Before the first sample there is no count to give, so it says
/// what is happening instead.
[[nodiscard]] inline std::string processCountText(std::size_t count)
{
    if (count == 0)
    {
        return "Collecting processes\xE2\x80\xA6";
    }
    return std::format("{} {}", UI::Format::formatIntLocalized(count), (count == 1) ? "process" : "processes");
}

/// "Updates every 1 s"; "every 250 ms"; "every 1250 ms". The interval is the refresh rate set in
/// Settings, which each sampler follows. Whole seconds read as seconds, as the presets are named;
/// anything else stays in milliseconds, so the cadence shown is exact rather than rounded to a
/// decimal place (#1200 review: 1001 ms read "1.0 s").
[[nodiscard]] inline std::string updateIntervalText(int intervalMs)
{
    if (intervalMs <= 0)
    {
        return {};
    }
    if (intervalMs % 1000 == 0)
    {
        return std::format("Updates every {} s", intervalMs / 1000);
    }
    return std::format("Updates every {} ms", intervalMs);
}

/// @p text cut short to fit @p budgetPx: the longest prefix, ended on a UTF-8 character boundary,
/// whose "<prefix>…" fits, or @p text whole if it already fits. When not even the ellipsis fits,
/// nothing: drawing it anyway would overrun the space it was cut for (#1200 review).
///
/// @param measure  The pixel width of a string, e.g. ImGui::CalcTextSize(...).x.
template<typename Measure> [[nodiscard]] std::string ellipsize(std::string_view text, float budgetPx, const Measure& measure)
{
    if (measure(text) <= budgetPx)
    {
        return std::string(text);
    }
    constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";
    std::string candidate;
    candidate.reserve(text.size() + ELLIPSIS.size());
    // Longest first; a status segment is a few dozen bytes, so a linear walk is plenty.
    for (std::size_t cut = text.size(); cut > 0; --cut)
    {
        if (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
        {
            continue; // inside a character
        }
        candidate.assign(text.substr(0, cut)).append(ELLIPSIS);
        if (measure(candidate) <= budgetPx)
        {
            return candidate;
        }
    }
    candidate.assign(ELLIPSIS);
    if (measure(candidate) <= budgetPx)
    {
        return candidate;
    }
    return {};
}

/// How much of the status bar's text fits the space it has.
struct StatusBarFit
{
    std::size_t segmentsShown = 0; ///< Leading segments, in priority order, drawn whole.
    bool truncateFirst = false;    ///< Not even the first fits: draw it cut short with an ellipsis.
    bool showReadout = false;      ///< The right-aligned readout (FPS) fits as well.
};

/// Fit the status text and the right-aligned readout into @p budgetPx, without drawing one over the
/// other or past the bar's edge (#1200, #1207).
///
/// The segments are in priority order and are dropped from the end, whole, until the rest fit; if
/// not even the first does, it is kept and cut short instead, so the bar never goes blank. The
/// readout, the least important, is shown only when every segment is and there is still room for it
/// and the gap before it.
///
/// @param segmentWidthsPx  Width of each segment, most important first.
/// @param separatorPx      Width of SEPARATOR, drawn between two shown segments.
/// @param readoutPx        Width of the readout; 0 when there is none to show.
/// @param gapPx            Least space between the text and the readout.
/// @param budgetPx         Width from where the text starts to where the readout must end.
[[nodiscard]] inline StatusBarFit
fitStatusBar(std::span<const float> segmentWidthsPx, float separatorPx, float readoutPx, float gapPx, float budgetPx) noexcept
{
    const auto clean = [](float value)
    {
        return (std::isfinite(value) && value > 0.0F) ? value : 0.0F;
    };
    const float budget = clean(budgetPx);
    const float separator = clean(separatorPx);

    StatusBarFit fit;
    float used = 0.0F;
    for (const float width : segmentWidthsPx)
    {
        const float needed = ((fit.segmentsShown > 0) ? separator : 0.0F) + clean(width);
        if (used + needed > budget)
        {
            break;
        }
        used += needed;
        ++fit.segmentsShown;
    }
    if (fit.segmentsShown == 0 && !segmentWidthsPx.empty() && budget > 0.0F)
    {
        fit.truncateFirst = true;
        return fit;
    }

    const float readout = clean(readoutPx);
    fit.showReadout = (readout > 0.0F) && (fit.segmentsShown == segmentWidthsPx.size()) && (used + clean(gapPx) + readout <= budget);
    return fit;
}

} // namespace App::StatusBarText
