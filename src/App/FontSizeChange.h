#pragma once

#include "App/UserConfig.h"
#include "UI/FontPointSize.h"
#include "UI/Theme.h"

namespace App
{

/// The one way to change the UI font size at runtime, used by both the Settings dialog and the
/// Ctrl+= / Ctrl+- shortcuts (#1076): @p points, a body size in points, clamped to
/// [UI::FONT_POINT_SIZE_MIN, UI::FONT_POINT_SIZE_MAX] (#3375). Updates the saved setting and the theme;
/// UILayer sees the theme's size change at the next frame boundary and rebuilds the fonts there. Does
/// nothing if neither changes. No event is raised (#1178): font-dependent caches compare the font and
/// UI::Theme::fontGeneration() each frame and rebuild themselves, and nothing needs re-sampling.
inline void changeFontSize(int points)
{
    auto& settings = UserConfig::get().settings();
    auto& theme = UI::Theme::get();
    const int clamped = UI::clampFontPointSize(points);
    if (clamped == settings.fontPointSize && clamped == theme.fontPointSize())
    {
        return;
    }

    settings.fontPointSize = clamped;
    theme.setFontPointSize(clamped);
}

} // namespace App
