#pragma once

// The app's fonts baked into the current ImGui context's atlas and registered with Theme (#1547):
// Inter (regular, large and bold) with Font Awesome merged in, the Sixtyfour wordmark and the title bar's chrome icons, at every Font Size
// preset. Needs only an ImGui context, not a window or renderer: UILayer calls it at startup and on a display-scale change (#943), the
// headless PGO UI training driver (#880) loads the app's own fonts through it, and the real-Theme
// tests check it without a GL context.

#include "UI/FontFileCache.h"

#include <filesystem>

namespace UI
{

/// Bakes every font size preset at @p displayScale into the atlas and registers them with Theme. A
/// font file that cannot be read falls back to ImGui's embedded font (or, for the bold face, the
/// wordmark and the chrome icons, to none). @p fontFiles must outlive the atlas.
void loadAllFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale);

/// ImGui's embedded font at each preset's sizes, with Font Awesome merged in when it can be read;
/// for when loadAllFonts() threw. Cannot fail for want of the font files. @p fontFiles must outlive
/// the atlas.
void loadFallbackFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale);

} // namespace UI
