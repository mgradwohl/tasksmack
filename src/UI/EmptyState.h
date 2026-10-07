#pragma once

// A pane's "nothing to show" state, drawn as a deliberate, centred message instead of a line of
// text in the top-left corner of an otherwise blank window (#927). On a large window the corner
// label read as a broken screen rather than a state the application knew it was in.
//
// The pure arithmetic is separate from the drawing so it is unit-testable without a live ImGui
// context, following CONTRIBUTING.md's "extract the pure decision logic into a small header"
// pattern.

#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace UI::Widgets
{

/// Widest the explanatory text of an empty state runs before wrapping, in ems. About 60 characters:
/// long enough for a full sentence, short enough to read as a caption rather than a banner.
inline constexpr float EMPTY_STATE_WRAP_EM = 32.0F;

/// Gap between the heading and the explanatory text, in ems.
inline constexpr float EMPTY_STATE_GAP_EM = 0.75F;

/// Offset that centres `contentPx` within `availablePx`, never negative: content larger than the
/// space starts at the leading edge rather than being pushed off it.
[[nodiscard]] inline float centeredOffset(float availablePx, float contentPx) noexcept
{
    if (!std::isfinite(availablePx) || !std::isfinite(contentPx))
    {
        return 0.0F;
    }
    return std::max(0.0F, std::floor((availablePx - contentPx) * 0.5F));
}

/// Width the explanatory text wraps at: EMPTY_STATE_WRAP_EM, or the space available if narrower.
[[nodiscard]] inline float emptyStateWrapWidth(float emPx, float availableWidthPx) noexcept
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float wanted = EMPTY_STATE_WRAP_EM * em;
    if (!std::isfinite(availableWidthPx) || availableWidthPx <= 0.0F)
    {
        return wanted;
    }
    return std::min(wanted, availableWidthPx);
}

/// Slack added to a measured line width when it is reused as a wrap width, in pixels: enough that
/// the longest line does not re-wrap over float rounding, far too little to admit another word.
inline constexpr float EMPTY_STATE_WRAP_SLACK_PX = 1.0F;

/// Wrap extent, from the block's left edge, that reproduces line breaks measured at
/// `longestLinePx`. See renderEmptyState() for why this, and not the full wrap width, is used.
[[nodiscard]] inline float emptyStateWrapExtent(float longestLinePx) noexcept
{
    const float line = (std::isfinite(longestLinePx) && longestLinePx > 0.0F) ? longestLinePx : 0.0F;
    return std::ceil(line) + EMPTY_STATE_WRAP_SLACK_PX;
}

/// Height of an empty state drawn as a band within a pane (renderEmptyStateBand()), in ems: room for
/// the heading and a couple of lines of explanation with space around them.
inline constexpr float EMPTY_STATE_BAND_EM = 6.0F;

/// Draws an empty state centred in a region `regionHeight` tall starting at the cursor, as wide as
/// the remaining content region, and returns the height of the heading and explanation block.
/// renderEmptyState() and renderEmptyStateBand() are the two ways to call it.
inline float drawEmptyStateIn(const char* heading, const char* detail, float regionHeight)
{
    const bool hasDetail = (detail != nullptr) && (detail[0] != '\0');
    const auto& scheme = Theme::get().scheme();

    const ImVec2 origin = ImGui::GetCursorPos();
    const ImVec2 avail(ImGui::GetContentRegionAvail().x, regionHeight);
    const float emPx = ImGui::GetFontSize();
    const float wrapWidth = emptyStateWrapWidth(emPx, avail.x);
    const float gap = hasDetail ? (EMPTY_STATE_GAP_EM * emPx) : 0.0F;

    const ImVec2 headingSize = ImGui::CalcTextSize(heading);
    const ImVec2 detailSize = hasDetail ? ImGui::CalcTextSize(detail, nullptr, false, wrapWidth) : ImVec2(0.0F, 0.0F);
    const float blockHeight = headingSize.y + gap + detailSize.y;

    const float top = origin.y + centeredOffset(avail.y, blockHeight);

    ImGui::SetCursorPos(ImVec2(origin.x + centeredOffset(avail.x, headingSize.x), top));
    ImGui::TextColored(scheme.textPrimary, "%s", heading);

    if (hasDetail)
    {
        // The wrapped block is centred as a whole; its lines are left-aligned within it.
        //
        // detailSize.x is the width of the longest wrapped line, which is what gets centred -- so a
        // one-line explanation sits under its heading rather than at the left of a wider box. The
        // wrap position must then be measured from that same width, not from wrapWidth: adding the
        // full wrapWidth to an offset computed from a narrower block put the wrap point past the
        // pane's right edge on a narrow pane, where the text was clipped instead of wrapped.
        // Wrapping at the longest line's own width reproduces the measured line breaks exactly,
        // since no line was wider than that.
        const float detailLeft = origin.x + centeredOffset(avail.x, detailSize.x);
        ImGui::SetCursorPos(ImVec2(detailLeft, top + headingSize.y + gap));
        ImGui::PushTextWrapPos(detailLeft + emptyStateWrapExtent(detailSize.x));
        ImGui::TextColored(scheme.textMuted, "%s", detail);
        ImGui::PopTextWrapPos();
    }
    return blockHeight;
}

/// Draws an empty state centred in the remaining content region: a heading, and beneath it an
/// optional explanation of why the pane is empty and what to expect.
///
/// @param heading  What state this is, e.g. "No process selected". May start with an icon glyph.
/// @param detail   Why, and what happens next. Null or empty for none.
inline void renderEmptyState(const char* heading, const char* detail = nullptr)
{
    static_cast<void>(drawEmptyStateIn(heading, detail, ImGui::GetContentRegionAvail().y));
}

/// The same empty state for one section of a pane that has more below it (#1210): centred in a band
/// EMPTY_STATE_BAND_EM tall, or as tall as its text if that is more, with the cursor left after the
/// band so the rest of the pane follows it.
inline void renderEmptyStateBand(const char* heading, const char* detail = nullptr)
{
    const ImVec2 origin = ImGui::GetCursorPos();
    const float bandHeight = EMPTY_STATE_BAND_EM * ImGui::GetFontSize();
    const float blockHeight = drawEmptyStateIn(heading, detail, bandHeight);
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + std::max(bandHeight, blockHeight)));
    ImGui::Dummy(ImVec2(0.0F, 0.0F)); // Extends the window's content to the band's end
}

} // namespace UI::Widgets
