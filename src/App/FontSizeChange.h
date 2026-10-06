#pragma once

#include "App/UserConfig.h"
#include "UI/Theme.h"

namespace App
{

/// The one way to change the UI font size at runtime, used by both the Settings dialog and the
/// Ctrl+= / Ctrl+- shortcuts (#1076). Updates the saved setting and the theme. Does nothing if
/// neither changes. No event is raised (#1178): font-dependent caches compare the font and
/// UI::Theme::fontGeneration() each frame and rebuild themselves, and nothing needs re-sampling.
inline void changeFontSize(UI::FontSize size)
{
    auto& settings = UserConfig::get().settings();
    auto& theme = UI::Theme::get();
    if (size == settings.fontSize && size == theme.currentFontSize())
    {
        return;
    }

    settings.fontSize = size;
    theme.setFontSize(size);
}

} // namespace App
