#pragma once

// Pure colour arithmetic for choosing readable text on a filled control, extracted so it is
// unit-testable without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern (as DialogMetrics.h and StyleScale.h do).
//
// The formulas are WCAG 2's relative luminance and contrast ratio. They are used here only to pick
// the better of two candidate colours, not to certify a theme against a WCAG threshold.

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace UI::ColorContrast
{

/// One sRGB channel (0..1) converted to linear light.
[[nodiscard]] inline float linearChannel(float channel) noexcept
{
    const float c = std::isfinite(channel) ? std::clamp(channel, 0.0F, 1.0F) : 0.0F;
    return (c <= 0.04045F) ? (c / 12.92F) : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

/// Relative luminance of a colour, 0 (black) to 1 (white). Alpha is ignored: the callers pass
/// opaque fills and text colours.
[[nodiscard]] inline float relativeLuminance(const ImVec4& color) noexcept
{
    return (0.2126F * linearChannel(color.x)) + (0.7152F * linearChannel(color.y)) + (0.0722F * linearChannel(color.z));
}

/// Contrast ratio between two colours, 1 (identical luminance) to 21 (black on white).
[[nodiscard]] inline float contrastRatio(const ImVec4& a, const ImVec4& b) noexcept
{
    const float la = relativeLuminance(a);
    const float lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05F) / (std::min(la, lb) + 0.05F);
}

/// The opaque colour that results from drawing `top` over an opaque `bottom`.
///
/// Used to make popups opaque without changing how they look (#969). Every bundled theme gives
/// popup_background 94% alpha, and the 6% that showed through a modal was whatever lay underneath,
/// usually dense text. Simply forcing the alpha to 1 would remove that -- but it would also change
/// the colour: in several light themes the popup colour is the same as the button and combo colour,
/// and it was only the darkened backdrop showing through that kept those controls visible against
/// the dialog. Flattening the popup colour over the backdrop it is normally seen against keeps the
/// colour the theme's author was looking at, and makes it opaque.
///
/// `bottom`'s own alpha is ignored; the result is always fully opaque.
[[nodiscard]] inline ImVec4 flattenOver(const ImVec4& top, const ImVec4& bottom) noexcept
{
    const float alpha = std::isfinite(top.w) ? std::clamp(top.w, 0.0F, 1.0F) : 1.0F;
    const auto mix = [alpha](float over, float under)
    {
        return (over * alpha) + (under * (1.0F - alpha));
    };
    return {mix(top.x, bottom.x), mix(top.y, bottom.y), mix(top.z, bottom.z), 1.0F};
}

/// How much more contrast the alternate colour must offer before readableTextOn() switches to it.
inline constexpr float ALTERNATE_MARGIN = 1.2F;

/// Whichever of two candidate text colours reads better on `fill`.
///
/// A filled button was labelled in the theme's ordinary text colour whatever its fill. That works
/// when the fill is on the same side of mid-grey as the window background the text colour was
/// chosen for, and fails when it is not: on Solarized Light the dark text landed on a dark green
/// "Apply" button and was nearly unreadable (#969). A theme's text colour and its window background
/// are the two poles of its palette, so offering both and taking the one with more contrast gives a
/// readable label on any fill without a new colour in every theme file.
///
/// Call it with the fill actually on screen. A button has three -- resting, hovered, pressed -- and
/// themes move them in different directions; a colour chosen against the resting fill can be the
/// worse one on the hovered fill, which is the state the user is in when about to click. See
/// UI::Widgets::filledButton(), which chooses per state.
///
/// The alternate has to be clearly better to win, not merely better. Several bundled themes have a
/// mid-green fill on which the two candidates differ by a few percent (Tokyo Night: 3.25 against
/// 3.26); flipping those labels from light to dark would change an established look for no gain in
/// legibility.
///
/// @param fill       The control's background.
/// @param preferred  The colour to keep unless the alternate is clearly more readable.
/// @param alternate  The other candidate.
[[nodiscard]] inline ImVec4 readableTextOn(const ImVec4& fill, const ImVec4& preferred, const ImVec4& alternate) noexcept
{
    return (contrastRatio(fill, alternate) > (contrastRatio(fill, preferred) * ALTERNATE_MARGIN)) ? alternate : preferred;
}

/// Contrast floor for text (WCAG AA for normal text), as the bundled-theme tests hold text to.
inline constexpr float TEXT_CONTRAST_MIN = 4.5F;

/// How finely readableTint() mixes toward the readable colour: in steps of 1 / READABLE_TINT_STEPS.
inline constexpr int READABLE_TINT_STEPS = 20;

/// @p color as text on @p background: unchanged when it already reaches @p minRatio there, else
/// mixed toward @p readable (a colour that does, such as the theme's primary text) just far enough to
/// -- in steps of 1 / READABLE_TINT_STEPS of the way -- so it keeps as much of its hue as the floor allows. @p readable
/// itself if no mix gets there. A series colour is held only to 3:1, enough for a line but not for
/// the text that names it, such as a second axis's tick labels drawn in its series' colour (#1206).
[[nodiscard]] inline ImVec4 readableTint(const ImVec4& color, const ImVec4& readable, const ImVec4& background, float minRatio) noexcept
{
    if (contrastRatio(color, background) >= minRatio)
    {
        return color;
    }
    for (int step = 1; step < READABLE_TINT_STEPS; ++step)
    {
        const float t = static_cast<float>(step) / static_cast<float>(READABLE_TINT_STEPS);
        const ImVec4 mix{color.x + ((readable.x - color.x) * t),
                         color.y + ((readable.y - color.y) * t),
                         color.z + ((readable.z - color.z) * t),
                         color.w + ((readable.w - color.w) * t)};
        if (contrastRatio(mix, background) >= minRatio)
        {
            return mix;
        }
    }
    return readable;
}

} // namespace UI::ColorContrast
