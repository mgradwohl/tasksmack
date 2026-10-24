#pragma once

#include "App/UserConfig.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace App
{

/// The one way to change the theme at runtime, used by the Settings dialog's Apply and by
/// TASKSMACK_DEMO's theme= step (#2827). Records @p id in the settings and queues it on UI::Theme,
/// which applies it at the next frame boundary (Theme::applyPendingStyleChanges()), so the next frame
/// renders with it; nothing needs notifying or re-sampling (#1178). Does nothing when @p id is the
/// theme in use. Returns false, and changes nothing, when @p id names no loaded theme.
[[nodiscard]] inline bool changeTheme(const std::string_view id)
{
    auto& theme = UI::Theme::get();
    if (!std::ranges::any_of(theme.discoveredThemes(), [id](const UI::DiscoveredTheme& discovered) { return discovered.id == id; }))
    {
        return false;
    }
    auto& settings = UserConfig::get().settings();
    if (id == settings.themeId)
    {
        return true;
    }
    settings.themeId = std::string(id);
    theme.setThemeById(id);
    spdlog::info("Theme changed to {}", id);
    return true;
}

} // namespace App
