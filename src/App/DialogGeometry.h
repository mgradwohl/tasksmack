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

// ---- About box (#935, redesigned #1490) ----

/// Margin around the dialog's contents. Was 3 em (a 24pt value once converted through a hardcoded
/// 96.0F / 72.0F); halved in the compact redesign so the margin no longer dwarfs the content (#1490).
inline constexpr float ABOUT_MARGIN_EM = 1.5F; // 16px at REFERENCE_EM_PX

/// Longest edge of the application icon. Was 9 em (96px), taller than the name, version and tagline
/// beside it; now about their height (#1490).
inline constexpr float ABOUT_ICON_EM = 6.0F; // 64px

/// Gap between the icon and the text beside it.
inline constexpr float ABOUT_HEADER_GAP_EM = 1.5F; // 16px

/// Narrowest the text beside the icon may be (or the title's width, if wider) before the header
/// stacks the icon above the text instead (#1490 review).
inline constexpr float ABOUT_HEADER_MIN_TEXT_EM = 12.0F; // 128px

/// Authored width of the dialog, applied every frame so the wrapped shortcut table has a fixed width
/// to wrap to. It used to auto-fit, and the table's wrapped text fed back into the fit, widening the
/// dialog a little every frame until it filled most of the window (#1490).
inline constexpr float ABOUT_WIDTH_EM = 36.0F; // 384px

/// Tallest the dialog may be, as a share of the window's height. Its contents scroll inside it past
/// that, rather than the dialog growing to the shared 90 % cap and covering the app (#1490).
inline constexpr float ABOUT_MAX_HEIGHT_FRACTION = 0.7F;

/// Floor on the OK button's width.
inline constexpr float ABOUT_BUTTON_MIN_EM = 11.25F; // 120px

// ---- Elevation notice (#937) ----

/// Authored width of the notice. Like the About box (#1490), this width is applied every
/// frame the popup is open -- see ElevationNoticeLayer for why that distinction matters.
inline constexpr float ELEVATION_WIDTH_EM = 45.0F; // 480px

/// Floor on the OK button's width.
inline constexpr float ELEVATION_BUTTON_MIN_EM = 9.375F; // 100px

// ---- Settings dialog (#921) ----

/// Floor on the Cancel/Apply buttons' width. The dialog's columns are measured from the text they
/// hold rather than authored, so this is the only fixed-pixel value it had left.
inline constexpr float SETTINGS_BUTTON_MIN_EM = 9.375F; // 100px

/// Smallest a Settings combo may be squeezed to, over and above its frame padding and arrow, when a
/// very long user-supplied theme name would otherwise push the dialog past the viewport. Roughly six
/// characters of preview text -- enough to stay recognisably a combo rather than a stub.
inline constexpr float MIN_COMBO_EM = 6.0F;

} // namespace App
