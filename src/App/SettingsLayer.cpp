#include "SettingsLayer.h"

#include "App/FontSizeChange.h"
#include "App/PlatformOpen.h"
#include "App/SelectOverride.h"
#include "App/SettingsDialog.h"
#include "App/SettingsLayerDetail.h"
#include "App/ThemeChange.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace App
{

using Detail::FONT_SIZE_OPTIONS;
using Detail::HISTORY_OPTIONS;
using Detail::pickedOption;
using Detail::REFRESH_RATE_OPTIONS;

SettingsLayer::SettingsLayer() : Core::Layer("SettingsLayer")
{}

SettingsLayer::SettingsLayer(std::function<void(Core::Event&)> raiseEvent,
                             std::function<bool(const std::filesystem::path&)> openPath,
                             std::function<std::filesystem::path()> userConfigDir)
    : Core::Layer("SettingsLayer"),
      m_RaiseEvent(std::move(raiseEvent)),
      m_OpenPath(std::move(openPath)),
      m_UserConfigDir(std::move(userConfigDir))
{}

void SettingsLayer::raiseEvent(Core::Event& event) const
{
    if (m_RaiseEvent)
    {
        m_RaiseEvent(event);
        return;
    }
    Core::Application::get().raiseEvent(event);
}

void SettingsLayer::openPath(const std::filesystem::path& path) const
{
    // Result intentionally ignored - openWithSystemHandler logs warnings on failure
    if (m_OpenPath)
    {
        static_cast<void>(m_OpenPath(path));
        return;
    }
    static_cast<void>(App::PlatformOpen::openWithSystemHandler(path));
}

// The user themes directory: the one UILayer scans and merges over the built-ins. The built-in
// themes live in the install's assets directory, which is read-only and replaced on upgrade, so
// that is not where users should add themes (#1127). Created on first use so the button always
// opens something.
auto SettingsLayer::userThemesDir() const -> std::filesystem::path
{
    auto dir = (m_UserConfigDir ? m_UserConfigDir() : Core::Application::get().paths().userConfigDir()) / "themes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        spdlog::warn("Couldn't create user themes directory {}: {}", Core::pathToUtf8(dir), ec.message());
    }
    return dir;
}

SettingsLayer::~SettingsLayer() = default;

void SettingsLayer::onAttach()
{
    // TASKSMACK_OPEN=settings (#2827): open at startup, for an unattended capture. The themes the dialog
    // lists are loaded by UILayer, pushed before this layer (main.cpp).
    if (SelectOverride::startupDialog() == SelectOverride::StartupDialog::Settings)
    {
        requestOpen();
    }
}

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
    {
        // On a first run nothing has saved the file yet, and opening a path that doesn't exist does
        // nothing the user can see (#2145): write it first, every key with its documentation. The one
        // synchronous save a click makes (#2916): the editor opens the file at once, so it must exist
        // first, and it is written at most once, on a first run.
        auto& config = UserConfig::get();
        std::error_code ec;
        if (!std::filesystem::exists(config.configPath(), ec))
        {
            config.save();
            if (!std::filesystem::exists(config.configPath(), ec))
            {
                spdlog::warn("Couldn't create {} to open it", Core::pathToUtf8(config.configPath()));
            }
        }
        openPath(config.configPath());
        break;
    }
    case SettingsDialog::Action::OpenThemesFolder:
        openPath(userThemesDir());
        break;
    case SettingsDialog::Action::OpenAbout:
    {
        Core::OpenAboutEvent event;
        raiseEvent(event);
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
    // TASKSMACK_DEMO's close=dialog (#2827): closes the dialog as Cancel does, on the next render.
    dispatcher.dispatch<Core::CloseDialogEvent>(
        [this](Core::CloseDialogEvent&)
        {
            m_State.closeRequested = true;
            return false; // The Help window and the About dialog close on it too
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

    // Only the combos the user picked are written (#1120, #1151): see Detail::pickedOption.

    // Apply theme: the next frame renders with it; nothing needs notifying or re-sampling (#1178).
    // The same path TASKSMACK_DEMO's theme= takes (ThemeChange.h, #2827).
    if (m_State.themeChoice.touched && m_State.themeChoice.index.has_value() && *m_State.themeChoice.index < m_State.themes.size())
    {
        static_cast<void>(changeTheme(m_State.themes[*m_State.themeChoice.index].id)); // from the loaded list, so never unknown
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
            raiseEvent(event);
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
            raiseEvent(event);
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

    // Opt-in UDP event trace (#1492, Windows): the Processes panel starts or stops it now
    if (m_State.udpEventTrace != settings.udpEventTrace)
    {
        settings.udpEventTrace = m_State.udpEventTrace;
        {
            Core::UdpEventTraceChangedEvent event(settings.udpEventTrace);
            raiseEvent(event);
        }
        spdlog::info("Settings: Count UDP traffic per process changed to {}", settings.udpEventTrace);
    }

    // Process icons (#1800): the panels read the setting every frame, so it shows at once
    if (m_State.showProcessIcons != settings.showProcessIcons)
    {
        settings.showProcessIcons = m_State.showProcessIcons;
        spdlog::info("Settings: Show process icons changed to {}", m_State.showProcessIcons);
    }

    // Save to disk, on a worker: the UI thread never writes the file (#2916)
    config.saveInBackground();
}

} // namespace App
