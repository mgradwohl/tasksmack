#pragma once

#include "App/UserConfig.h"
#include "UI/Theme.h"

namespace App
{

/// The one way to change the Interface scale at runtime (#3376): @p percent in 100-300, or 0 for Auto. Updates the
/// saved setting and the theme; UILayer sees the theme's value change at the next frame boundary and rebuilds the
/// fonts and style at the new scale, as it does when the display's own scale changes. Does nothing if neither
/// changes.
inline void changeInterfaceScale(int percent)
{
    auto& settings = UserConfig::get().settings();
    auto& theme = UI::Theme::get();
    if (percent == settings.interfaceScalePercent && percent == theme.interfaceScalePercent())
    {
        return;
    }
    settings.interfaceScalePercent = percent;
    theme.setInterfaceScalePercent(percent);
}

} // namespace App
