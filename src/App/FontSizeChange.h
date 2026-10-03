#pragma once

#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "UI/Theme.h"

namespace App
{

/// The one way to change the UI font size at runtime, used by both the Settings dialog and the
/// Ctrl+= / Ctrl+- shortcuts (#1076). Updates the saved setting and the theme, then raises
/// FontSizeChangedEvent so font-dependent caches rebuild. Does nothing if neither changes.
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
    Core::FontSizeChangedEvent event(static_cast<int>(size));
    Core::Application::get().raiseEvent(event);
}

} // namespace App
