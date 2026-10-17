#pragma once

#include "App/ElevationNoticeText.h"

#include <string>
#include <string_view>

namespace App::ElevationNoticeDialog
{

/// The notice's popup ID (and its title).
inline constexpr const char* POPUP_ID = "Limited Data Available";

/// What the notice keeps between frames.
struct State
{
    bool openRequested = false; ///< Set to open it on the next frame; cleared when it opens
    bool dontShowAgain = false; ///< The "Don't show again" checkbox
    /// The body: what is missing (#2303). Empty: the platform's text with nothing more known.
    std::string bodyText;
    // The viewport size and dialog width it was last centred for: either changing re-centres it (#1601).
    float centredViewportWidth = 0.0F;
    float centredViewportHeight = 0.0F;
    float centredDialogWidth = 0.0F;
    float dialogHeight = 0.0F;        ///< The dialog's height last frame (it auto-fits a frame behind)
    float centredDialogHeight = 0.0F; ///< The height it was last centred with
};

/// Draw the Limited Data notice for this frame: open it when @p state.openRequested is set, then draw
/// it while it is open, fitted to its text and centred on the window.
///
/// Split out of ElevationNoticeLayer, which keeps the event wiring and the "Don't show again" save, so
/// the dialog can be rendered headless in the tests (#1547; CONTRIBUTING.md, "Testing App/UI code that
/// needs a live ImGui context").
///
/// @return true on the frame OK closed it; @p state.dontShowAgain then says whether to stop showing it.
[[nodiscard]] bool render(State& state);

/// Width the dialog's content needs unwrapped, window padding and title bar included (#1601), with
/// @p bodyText as its body.
[[nodiscard]] float measureContentWidth(std::string_view bodyText);

/// The body @p state shows: its bodyText, or the platform's text when that is empty.
[[nodiscard]] std::string bodyTextOf(const State& state);

/// The dialog's width this frame, with @p bodyText as its body: fitted to its text between its floor
/// and authored width (#1601), clamped to the viewport, and -- where the body has to wrap -- moved
/// by up to a word either way so no paragraph ends in a lone word (#1986). A whole number of pixels
/// wherever it matters: ImGui truncates a window's width, and a text that just fit then wrapped its
/// last word. Needs a frame (it measures text in the current font).
[[nodiscard]] float computeWidth(std::string_view bodyText, float viewportWidthPx);

} // namespace App::ElevationNoticeDialog
