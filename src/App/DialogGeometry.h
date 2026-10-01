#pragma once

// Per-dialog geometry, in ems, for the application's modal dialogs.
//
// These live in a header rather than beside each dialog so that the reference-geometry tests can
// assert the values the dialogs actually use. A test that supplies its own literal proves only that
// the arithmetic in UI/DialogMetrics.h works; it stays green while a call-site constant is changed
// and the dialog quietly restyles, which is exactly the regression the tests are meant to catch.
//
// Each value was derived from the fixed pixel size it replaced, so every dialog renders unchanged at
// the reference configuration and scales from there. See UI/DialogMetrics.h for how they are used
// and why one em is the right unit.

namespace App
{

/// One em at the reference configuration: the Medium preset's 8pt body font on a 1.0 display scale.
/// At 96 DPI that is 8 * 96/72 = 32/3 px. Every constant below was derived against this, and the
/// tests assert each one still reproduces the pixel size named in its comment.
inline constexpr float REFERENCE_EM_PX = 32.0F / 3.0F;

// ---- About box (#935) ----

/// Margin around the dialog's contents. Replaces a 24pt value converted through a hardcoded
/// 96.0F / 72.0F and scaled by io.FontGlobalScale.
inline constexpr float ABOUT_MARGIN_EM = 3.0F; // 32px at REFERENCE_EM_PX

/// Longest edge of the application icon.
inline constexpr float ABOUT_ICON_EM = 9.0F; // 96px

/// Floor on the OK button's width.
inline constexpr float ABOUT_BUTTON_MIN_EM = 11.25F; // 120px

// ---- Elevation notice (#937) ----

/// Authored width of the notice. Unlike the About box, which auto-fits, this width is applied every
/// frame the popup is open -- see ElevationNoticeLayer for why that distinction matters.
inline constexpr float ELEVATION_WIDTH_EM = 45.0F; // 480px

/// Floor on the OK button's width.
inline constexpr float ELEVATION_BUTTON_MIN_EM = 9.375F; // 100px

} // namespace App
