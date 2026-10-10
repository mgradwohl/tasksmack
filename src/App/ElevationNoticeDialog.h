#pragma once

namespace App::ElevationNoticeDialog
{

/// The notice's popup ID (and its title).
inline constexpr const char* POPUP_ID = "Limited Data Available";

/// What the notice keeps between frames.
struct State
{
    bool openRequested = false; ///< Set to open it on the next frame; cleared when it opens
    bool dontShowAgain = false; ///< The "Don't show again" checkbox
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

/// Width the dialog's content needs unwrapped, window padding and title bar included (#1601).
[[nodiscard]] float measureContentWidth();

} // namespace App::ElevationNoticeDialog
