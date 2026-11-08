#include "SettingsLayer.h"

#include "App/BackgroundOpen.h"
#include "App/FontSizeChange.h"
#include "App/InterfaceScaleChange.h"
#include "App/PanelPlatforms.h"
#include "App/PlatformOpen.h"
#include "App/SelectOverride.h"
#include "App/SettingsDialog.h"
#include "App/SettingsLayerDetail.h"
#include "App/StartAtLoginSetting.h"
#include "App/SyntheticScenario.h"
#include "App/TaskManagerReplacementController.h"
#include "App/ThemeChange.h"
#include "App/UdpCountingStatus.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Domain/StartAtLogin.h"
#include "Platform/HostEnvironment.h"
#include "Platform/IStartAtLogin.h"
#include "UI/Theme.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace App
{

using Detail::FONT_SIZE_OPTIONS;
using Detail::HISTORY_OPTIONS;
using Detail::INTERFACE_SCALE_OPTIONS;
using Detail::pickedOption;
using Detail::REFRESH_RATE_OPTIONS;

// The sign-in entry (#3093 part 2) is made here, at construction: making it reads nothing. None in a
// synthetic or test-hook run.
SettingsLayer::SettingsLayer()
    : Core::Layer("SettingsLayer"), m_StartAtLogin(makeStartAtLoginEntry(Synthetic::activeScenario(), SelectOverride::testHookActive()))
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

void SettingsLayer::startOpen(std::function<void()> prepare, std::filesystem::path path)
{
    // ShellExecuteW or xdg-open, and what must be on disk first, on the worker: never on the UI thread
    // (#3155). Its result is ignored here, as before: the opener logs a failure. The job holds a copy of
    // the test seam (empty outside the tests) and picks the opener itself.
    // The analyzer reports the job's capture list (the std::functions moved into it) as a leak; the
    // job, which BackgroundTask owns, owns them. A block, not NOLINTNEXTLINE: the report's line is the
    // capture list's, inside the call.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
    static_cast<void>(m_Open.start(
        [prepare = std::move(prepare), path = std::move(path), seam = m_OpenPath]
        {
            prepare();
            return seam ? seam(path) : App::PlatformOpen::openWithSystemHandler(path);
        }));
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
}

void SettingsLayer::waitForPendingOpen()
{
    m_Open.wait();
    static_cast<void>(m_Open.poll());
}

void SettingsLayer::setStartAtLoginEntry(std::shared_ptr<Platform::IStartAtLogin> entry)
{
    m_StartAtLogin = StartAtLoginSetting(std::move(entry));
    syncStartAtLogin();
}

void SettingsLayer::waitForStartAtLoginForTesting()
{
    m_StartAtLogin.waitForTesting();
    syncStartAtLogin();
}

void SettingsLayer::syncStartAtLogin()
{
    static_cast<void>(m_StartAtLogin.poll());
    m_State.startAtLoginOffered = m_StartAtLogin.offered();
    m_State.startAtLoginBusy = m_StartAtLogin.busy();
    m_State.startAtLoginChanging = m_StartAtLogin.changing();
    // A new status puts the checkbox on it: what the entry is now, after a read, a Save or a Repair.
    if (m_StartAtLogin.status() != m_State.startAtLoginStatus)
    {
        m_State.startAtLoginStatus = m_StartAtLogin.status();
        m_State.startAtLogin = m_State.startAtLoginStatus.enabled();
        rememberStartAtLoginStatus(m_State.startAtLoginStatus);
    }
}

void SettingsLayer::rememberStartAtLoginStatus(const Domain::StartAtLogin::Status& status)
{
    namespace StartAtLogin = Domain::StartAtLogin;
    if (status.state == StartAtLogin::State::Unknown || !m_StartAtLogin.restoresMissingEntry())
    {
        return;
    }
    const bool startupCheck = std::exchange(m_StartupCheckPending, false);
    if (startupCheck)
    {
        takeStartupUninstalled(status.uninstalled);
    }
    if (status.state == StartAtLogin::State::On)
    {
        rememberStartAtLogin(true); // on for this copy, now seen by this build
        return;
    }
    // The startup check found it gone, and didn't write it back: TaskSmack was uninstalled since, or it was
    // removed by hand while this build ran it (StartAtLogin::startupAction()'s Forget), so it stays off.
    const UserSettings& settings = UserConfig::get().settings();
    if (startupCheck && status.state == StartAtLogin::State::Off &&
        StartAtLogin::startupAction(
            settings.startAtSignIn, status.uninstalled, settings.startAtSignInSeenBy, buildIdentity(), true, status.installed, false) ==
            StartAtLogin::StartupAction::Forget)
    {
        spdlog::info("Start at sign-in was turned off outside TaskSmack: leaving it off");
        rememberStartAtLogin(false);
    }
}

void SettingsLayer::takeStartupUninstalled(bool uninstalled)
{
    m_StartupUninstalled = uninstalled;
    if (uninstalled)
    {
        // A real uninstall since the last run (#3276): what the user turned on went with it. Only these two
        // settings are forgotten; the rest of config.toml stays.
        spdlog::info("TaskSmack was uninstalled since it last ran: forgetting Start at sign-in and Replace Task Manager");
        auto& config = UserConfig::get();
        if (config.settings().replaceTaskManager)
        {
            config.settings().replaceTaskManager = false;
            config.saveInBackground();
        }
    }
    if (m_TaskManager != nullptr)
    {
        m_TaskManager->intentChecked(uninstalled);
    }
}

void SettingsLayer::rememberStartAtLogin(bool on)
{
    if (!m_StartAtLogin.restoresMissingEntry())
    {
        return;
    }
    auto& config = UserConfig::get();
    UserSettings& settings = config.settings();
    std::string seenBy = on ? buildIdentity() : std::string{};
    if (settings.startAtSignIn == on && settings.startAtSignInSeenBy == seenBy)
    {
        return;
    }
    settings.startAtSignIn = on;
    settings.startAtSignInSeenBy = std::move(seenBy);
    config.saveInBackground(); // on the config worker, never here (#2916)
}

// The user themes directory: the one UILayer scans and merges over the built-ins. The built-in
// themes live in the install's assets directory, which is read-only and replaced on upgrade, so
// that is not where users should add themes (#1127). Made by the open worker before it is opened, so
// the button always opens something.
auto SettingsLayer::userThemesDir() const -> std::filesystem::path
{
    return (m_UserConfigDir ? m_UserConfigDir() : Core::Application::get().paths().userConfigDir()) / "themes";
}

SettingsLayer::~SettingsLayer() = default;

void SettingsLayer::onAttach()
{
    // Asked once here, not per frame: under WSL the dialog explains Auto's 100% beside the Interface scale (#3376).
    m_State.underWsl = Platform::runningUnderWsl();
    // TASKSMACK_OPEN=settings (#2827): open at startup, for an unattended capture. The themes the dialog
    // lists are loaded by UILayer, pushed before this layer (main.cpp).
    if (SelectOverride::startupDialog() == SelectOverride::StartupDialog::Settings)
    {
        requestOpen();
    }
    // Start at sign-in's startup check (#3276), on its worker: an install that removed the Run value the user
    // had turned on (an upgrade to another folder, or from a build whose uninstaller removed it) has it
    // written back. Only where an install can remove it (Windows).
    if (m_StartAtLogin.restoresMissingEntry() && !m_StartAtLogin.busy())
    {
        const UserSettings& settings = UserConfig::get().settings();
        m_StartupCheckPending = true;
        m_StartAtLogin.startupCheck(settings.startAtSignIn, settings.startAtSignInSeenBy, buildIdentity());
    }
}

void SettingsLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void SettingsLayer::onHiddenUpdate()
{
    syncStartAtLogin();
    if (m_TaskManager != nullptr)
    {
        m_TaskManager->poll();
    }
}

void SettingsLayer::onRender()
{
    // A finished open frees the buttons; a failure is in the log, as the opener writes it.
    static_cast<void>(m_Open.poll());
    m_State.openingPath = m_Open.busy();
    syncStartAtLogin();
    {
        UdpCountingStatus udp = udpCountingStatus(); // #3366
        m_State.udpDelivering = udp.delivering;
        m_State.udpUnsupported = udp.unsupported;
        m_State.udpProblem = std::move(udp.problem);
    }
    if (m_TaskManager != nullptr)
    {
        m_TaskManager->poll();
        m_State.taskManager = &m_TaskManager->view();
    }
    const SettingsDialog::Action action = SettingsDialog::render(m_State);
    // Replace Task Manager acts at once, on its own worker (#3072): a UAC prompt, then the helper.
    if (m_TaskManager != nullptr && m_State.taskManagerRequest != TaskManagerRequest::None)
    {
        m_TaskManager->request(std::exchange(m_State.taskManagerRequest, TaskManagerRequest::None));
    }
    switch (action)
    {
    case SettingsDialog::Action::Save:
        applySettings();
        break;
    case SettingsDialog::Action::EditConfig:
    {
        // On a first run nothing has saved the file yet, and opening a path that doesn't exist does
        // nothing the user can see (#2145): the worker writes it first, every key with its
        // documentation, from a copy of the settings taken here, and starts the editor only then.
        // Checked and written there, never here (#3155).
        auto& config = UserConfig::get();
        const std::filesystem::path path = config.configPath();
        startOpen(
            [&config, settings = config.settings(), path = path] // a copy of its own, movable
            {
                if (!config.writeIfMissing(settings))
                {
                    spdlog::warn("Couldn't create {} to open it", Core::pathToUtf8(path));
                }
            },
            path); // a copy: the lambda above holds its own (argument order is unspecified)
        break;
    }
    case SettingsDialog::Action::OpenThemesFolder:
    {
        const std::filesystem::path dir = userThemesDir();
        startOpen(
            [dir = dir] // a copy of its own, movable
            {
                // Made on first use; a failure is logged, and the path opened anyway (the handler
                // says what it can't open).
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                if (ec)
                {
                    spdlog::warn("Couldn't create user themes directory {}: {}", Core::pathToUtf8(dir), ec.message());
                }
            },
            dir); // a copy, as above
        break;
    }
    case SettingsDialog::Action::CheckForUpdates:
    {
        // The shell's update checker asks at once (#3093); its status comes back as UpdateCheckStatusEvent.
        Core::CheckForUpdatesEvent event;
        raiseEvent(event);
        break;
    }
    case SettingsDialog::Action::RepairStartAtLogin:
        // Now, on the worker (#3093 part 2): the entry is pointed at this copy and read back.
        m_StartAtLogin.setEnabled(true);
        rememberStartAtLogin(true);
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
    // What the update check is doing (#3093): whether its rows show, and Check now's state and result line.
    dispatcher.dispatch<Core::UpdateCheckStatusEvent>(
        [this](Core::UpdateCheckStatusEvent& e)
        {
            m_State.updateCheckOffered = e.offered();
            m_State.updateChecking = e.checking();
            m_State.updateSummary = e.summary();
            return false;
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
    // Start at sign-in (#3093 part 2): the checkbox starts on the last status, and the entry is read again
    // on the worker, in case it changed outside TaskSmack.
    m_State.startAtLogin = m_State.startAtLoginStatus.enabled();
    m_StartAtLogin.refresh();
    if (m_TaskManager != nullptr)
    {
        m_TaskManager->refresh(); // another tool may have changed it since
        m_State.taskManagerRow = {};
    }
}

void SettingsLayer::setTaskManagerReplacement(std::shared_ptr<TaskManagerReplacementController> controller)
{
    m_TaskManager = std::move(controller);
    m_State.taskManager = (m_TaskManager != nullptr) ? &m_TaskManager->view() : nullptr;
    // The restore notice waits for the startup check's word on an uninstall (#3276), or has it already.
    if (m_TaskManager != nullptr && m_StartupUninstalled.has_value())
    {
        m_TaskManager->intentChecked(*m_StartupUninstalled);
    }
    else if (m_TaskManager != nullptr && m_StartupCheckPending)
    {
        m_TaskManager->awaitIntentCheck();
    }
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
    if (const auto font = pickedOption(m_State.fontSizeChoice, FONT_SIZE_OPTIONS);
        font.has_value() && font->points != settings.fontPointSize)
    {
        changeFontSize(font->points); // UILayer rebuilds the fonts at it next frame (#3375)
        spdlog::info("Font size changed to {}", font->label);
    }

    // Apply the Interface scale (#3376): UILayer rebuilds the fonts and style at it next frame.
    if (const auto scale = pickedOption(m_State.interfaceScaleChoice, INTERFACE_SCALE_OPTIONS);
        scale.has_value() && scale->percent != settings.interfaceScalePercent)
    {
        changeInterfaceScale(scale->percent);
        spdlog::info("Interface scale changed to {}", scale->percent == 0 ? std::string("Auto") : std::format("{}%", scale->percent));
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

    // New-process highlighting (#3060): read by the Processes table every frame, so it shows at once
    if (m_State.highlightNewProcesses != settings.highlightNewProcesses)
    {
        settings.highlightNewProcesses = m_State.highlightNewProcesses;
        spdlog::info("Settings: Highlight new and exited processes changed to {}", m_State.highlightNewProcesses);
    }

    // The opt-in update check (#3093): the shell's checker reads the setting each frame, so a check that is
    // due starts at once, and turning it off stops any further request and hides the notice.
    if (m_State.checkForUpdates != settings.updates.enabled)
    {
        settings.updates.enabled = m_State.checkForUpdates;
        spdlog::info("Settings: Check for updates daily changed to {}", settings.updates.enabled);
    }

    // Start at sign-in (#3093 part 2): the entry is the setting. Turned on or off on its worker, only when the
    // box was changed from what was read; config.toml only remembers it was turned on (#3276).
    if (m_StartAtLogin.offered() && m_State.startAtLoginStatus.known() && m_State.startAtLogin != m_State.startAtLoginStatus.enabled())
    {
        m_StartAtLogin.setEnabled(m_State.startAtLogin);
        rememberStartAtLogin(m_State.startAtLogin); // and in config.toml, where an install can remove it (#3276)
    }

    // Save to disk, on a worker: the UI thread never writes the file (#2916)
    config.saveInBackground();
}

} // namespace App
