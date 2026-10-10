#include "SettingsLayer.h"

#include "App/FontSizeChange.h"
#include "App/PlatformOpen.h"
#include "App/SettingsDialog.h"
#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace App
{

using Detail::FONT_SIZE_OPTIONS;
using Detail::HISTORY_OPTIONS;
using Detail::pickedOption;
using Detail::REFRESH_RATE_OPTIONS;

namespace
{

// The user themes directory: the one UILayer scans and merges over the built-ins. The built-in
// themes live in the install's assets directory, which is read-only and replaced on upgrade, so
// that is not where users should add themes (#1127). Created on first use so the button always
// opens something.
[[nodiscard]] auto getUserThemesDir() -> std::filesystem::path
{
    auto dir = Core::Application::get().paths().userConfigDir() / "themes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        spdlog::warn("Couldn't create user themes directory {}: {}", dir.string(), ec.message());
    }
    return dir;
}

} // namespace

SettingsLayer::SettingsLayer() : Core::Layer("SettingsLayer")
{}

SettingsLayer::~SettingsLayer() = default;

void SettingsLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void SettingsLayer::onRender()
{
    switch (SettingsDialog::render(m_State))
    {
    case SettingsDialog::Action::Save:
        applySettings();
        break;
    case SettingsDialog::Action::EditConfig:
        // Result intentionally ignored - openWithSystemHandler logs warnings on failure
        (void) App::PlatformOpen::openWithSystemHandler(UserConfig::get().configPath());
        break;
    case SettingsDialog::Action::OpenThemesFolder:
        (void) App::PlatformOpen::openWithSystemHandler(getUserThemesDir());
        break;
    case SettingsDialog::Action::OpenAbout:
    {
        Core::OpenAboutEvent event;
        Core::Application::get().raiseEvent(event);
        break;
    }
    case SettingsDialog::Action::None:
        break;
    }
}

void SettingsLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    // Listen for settings dialog requests
    dispatcher.dispatch<Core::OpenSettingsEvent>(
        [this](Core::OpenSettingsEvent&)
        {
            requestOpen();
            return false; // Don't consume
        });
}

void SettingsLayer::requestOpen()
{
    m_State.openRequested = true;
    SettingsDialog::load(m_State, UserConfig::get().settings(), UI::Theme::get().discoveredThemes());
}

void SettingsLayer::applySettings()
{
    auto& config = UserConfig::get();
    auto& settings = config.settings();
    auto& themeManager = UI::Theme::get();

    // Only the combos the user picked are written (#1120, #1151): see Detail::pickedOption.

    // Apply theme
    if (m_State.themeChoice.touched && m_State.themeChoice.index.has_value() && *m_State.themeChoice.index < m_State.themes.size())
    {
        const std::string& newThemeId = m_State.themes[*m_State.themeChoice.index].id;
        if (newThemeId != settings.themeId)
        {
            settings.themeId = newThemeId;
            // The next frame renders with it; nothing needs notifying or re-sampling (#1178).
            themeManager.setThemeById(newThemeId);
            spdlog::info("Theme changed to {}", newThemeId);
        }
    }

    // Apply font size
    if (const auto font = pickedOption(m_State.fontSizeChoice, FONT_SIZE_OPTIONS); font.has_value() && font->value != settings.fontSize)
    {
        changeFontSize(font->value);
        spdlog::info("Font size changed to {}", font->label);
    }

    // Apply refresh rate
    if (const auto refresh = pickedOption(m_State.refreshRateChoice, REFRESH_RATE_OPTIONS);
        refresh.has_value() && refresh->valueMs != settings.refreshIntervalMs)
    {
        const int newRefreshMs = refresh->valueMs;
        settings.refreshIntervalMs = newRefreshMs;
        // Notify panels/samplers of interval change via event
        {
            Core::RefreshRateChangedEvent event(newRefreshMs);
            Core::Application::get().raiseEvent(event);
        }
        spdlog::info("Settings: Refresh rate changed to {} ms", newRefreshMs);
    }

    // Apply history duration
    if (const auto history = pickedOption(m_State.historyChoice, HISTORY_OPTIONS);
        history.has_value() && history->valueSeconds != settings.maxHistorySeconds)
    {
        const int newHistorySeconds = history->valueSeconds;
        settings.maxHistorySeconds = newHistorySeconds;
        // Notify panels/models to adjust history window via event
        {
            Core::HistoryDurationChangedEvent event(newHistorySeconds);
            Core::Application::get().raiseEvent(event);
        }
        spdlog::info("Settings: History duration changed to {} seconds", newHistorySeconds);
    }

    // Apply native-decorations preference (takes effect on next launch -- the window is
    // only created once, at startup)
    if (m_State.forceNativeDecorationsOnWayland != settings.forceNativeWindowDecorationsOnWayland)
    {
        settings.forceNativeWindowDecorationsOnWayland = m_State.forceNativeDecorationsOnWayland;
        spdlog::info("Settings: Force native window decorations on Wayland changed to {} (takes effect on next launch)",
                     m_State.forceNativeDecorationsOnWayland);
    }

    // Limited-data notice preference (read at startup, like the dialog's own "Don't show again")
    if (m_State.showPrivilegeNotice != settings.showPrivilegeNotice)
    {
        settings.showPrivilegeNotice = m_State.showPrivilegeNotice;
        spdlog::info("Settings: Show limited-data notice changed to {}", m_State.showPrivilegeNotice);
    }

    // Save to disk
    config.save();
}

} // namespace App
