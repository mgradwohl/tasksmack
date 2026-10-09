#ifdef _WIN32
#include <windows.h>
#endif

#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/InstanceLock.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/SyntheticScenario.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "App/WindowOverride.h"
#include "Core/Application.h"
#include "Core/ConfigDirOverride.h"
// NOLINTNEXTLINE(misc-include-cleaner) - used in the NDEBUG (release) branch below, invisible to debug-config analysis
#include "Core/EnvUtils.h"
#include "Core/WindowConstants.h"
#include "UI/UILayer.h"
#include "version.h"

#include <SDL3/SDL.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <utility>
#ifdef _WIN32
// spdlog's msvc_sink.h is itself guarded on _WIN32; the other sinks are portable.
#include <spdlog/sinks/msvc_sink.h>
#endif

#include <algorithm>
#include <clocale>
#include <cstdio> // NOLINT(misc-include-cleaner) - FILE* is used only in the _WIN32 console-attach branch below
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <locale>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace
{
void initializeLocale()
{
    // Use user-preferred locale ("" picks up OS locale) and force UTF-8 I/O where possible.
    try
    {
        const std::locale userLocale("");
        std::locale::global(userLocale);
        std::cout.imbue(userLocale);
        std::cerr.imbue(userLocale);
        std::cin.imbue(userLocale);
    }
    catch (const std::exception& e)
    {
        std::println(stderr, "Failed to set global locale: {}", e.what());
    }

    // Ensure C locale uses UTF-8
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - called once at startup before any threads are created
    setlocale(LC_ALL, "");

#ifdef _WIN32
    // On Windows, also set the console code page to UTF-8 for wide output.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

auto runApp() -> int
{
    initializeLocale();

// Required on Windows to see console output when launching from an IDE or debugger. Debug-only
// on purpose: a release-class GUI build must not pop up a console window.
#if defined(_WIN32) && !defined(NDEBUG)
    // Try to attach to parent console if it's a console app
    // If no parent console exists because it's a Windows app create our own console
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
    {
        AllocConsole();
        // Redirect stdout/stderr to the new console
        // NOLINTNEXTLINE(misc-const-correctness) - freopen_s writes to these pointers
        FILE* out = nullptr;
        // NOLINTNEXTLINE(misc-const-correctness) - freopen_s writes to these pointers
        FILE* err = nullptr;
        if (freopen_s(&out, "CONOUT$", "w", stdout) != 0)
        {
            // Redirection failed, but continue - spdlog will still work via msvc_sink
        }
        if (freopen_s(&err, "CONOUT$", "w", stderr) != 0)
        {
            // Redirection failed, but continue - spdlog will still work via msvc_sink
        }
    }
#endif

    // Load user configuration early so we can apply window geometry before creating the SDL window.
    auto& userConfig = App::UserConfig::get();

    // One TaskSmack per config directory (#1230): two instances saving the same config.toml could
    // undo each other's settings change. Held until runApp() returns; the OS drops it on any exit.
    // A lock that can't be taken at all (read-only directory, say) doesn't stop TaskSmack starting.
    // Taken before the file logger below opens tasksmack-debug.log, which truncates it: a rejected
    // second launch must not erase the running instance's log. Its warning goes to the default
    // (console) logger.
    const std::filesystem::path instanceLockPath = App::instanceLockPath(userConfig.configPath().parent_path());
    const App::InstanceLock instanceLock(instanceLockPath);
    if (instanceLock.status() == App::InstanceLock::Status::HeldByAnotherInstance)
    {
        spdlog::warn("TaskSmack is already running with the settings in {}; exiting", instanceLockPath.parent_path().string());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                                 "TaskSmack",
                                 "TaskSmack is already running.\n\nOnly one TaskSmack can run at a time, so that two can't overwrite "
                                 "each other's settings.",
                                 nullptr);
        return EXIT_SUCCESS;
    }

    // Logger construction runs in EVERY configuration. It used to sit inside the console-attach
    // guard above, so every NDEBUG build (win-release/win-optimized/win-profile) and every Linux
    // build fell back to spdlog's implicit default logger: no log file, no MSVC sink, and none of
    // this project's sink configuration. Because output still reached stdout, the app looked fine
    // and the loss was silent. Diagnostics matter most in exactly those builds (#915): on Windows
    // TaskSmack is a GUI-subsystem binary with no console of its own, so without a file sink the
    // only record of a run is a stdout stream the launcher had to redirect in advance -- a
    // TASKSMACK_TRACE_RESIZE_PERF capture started from Explorer discarded all of its evidence.
    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(3);
#ifdef _WIN32
    sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    std::filesystem::path logPath;
    try
    {
        logPath = std::filesystem::temp_directory_path() / "tasksmack-debug.log";
#ifdef _WIN32
        // Native wide path: the target defines SPDLOG_WCHAR_FILENAMES, so spdlog's filename_t is
        // std::wstring here. Passing a narrowed path would route through the active ANSI code
        // page and fail, or open the wrong file, whenever %TEMP% contains characters outside it.
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.wstring(), true));
#else
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true));
#endif
    }
    catch (const std::exception& e)
    {
        // Best-effort: keep the console (and MSVC) sinks. Clear logPath so the message below
        // cannot name a file that nothing is writing to.
        logPath.clear();
        std::println(stderr, "Failed to initialize file logging: {}", e.what());
    }
    catch (...)
    {
        logPath.clear();
        std::println(stderr, "Failed to initialize file logging (unknown error)");
    }

    spdlog::set_default_logger(std::make_shared<spdlog::logger>("TaskSmack", sinks.begin(), sinks.end()));

    if (!logPath.empty())
    {
        // u8string(), not string(): std::filesystem::path::string() performs a narrowing
        // conversion that can throw for a path the active code page cannot represent, and this
        // sits outside the try block above. UTF-8 bytes are lossless and cannot throw.
        const auto utf8Path = logPath.u8string();
        spdlog::info("Debug log file: {}", std::string(utf8Path.begin(), utf8Path.end()));
    }

    // Logged here rather than when it is read (UserConfig::get() above), so it reaches the log file.
    if (const auto& configDir = Core::ConfigDirOverride::active(); configDir.has_value())
    {
        const auto utf8Dir = configDir->u8string();
        spdlog::info("Config directory (test hook {}): {}", Core::ConfigDirOverride::ENV_VAR, std::string(utf8Dir.begin(), utf8Dir.end()));
    }

#ifndef NDEBUG
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::debug);
#else
    // In release builds, default to warn to silence info/debug noise while
    // preserving warnings, errors, and critical failures.
    // Override at runtime via TASKSMACK_LOG_LEVEL=trace|debug|info|warn|error|critical|off.
    // TASKSMACK_TRACE_RESIZE_PERF also promotes the level to info automatically.
    {
        spdlog::level::level_enum runtimeLevel = spdlog::level::warn;
        if (const char* levelEnv = SDL_getenv("TASKSMACK_LOG_LEVEL"); levelEnv != nullptr)
        {
            runtimeLevel = spdlog::level::from_str(levelEnv);
        }
        else if (Core::isEnvFlagEnabled(SDL_getenv("TASKSMACK_TRACE_RESIZE_PERF")))
        {
            runtimeLevel = spdlog::level::info;
        }
        spdlog::set_level(runtimeLevel);
    }
#endif

    spdlog::info("{} v{} ({} build)", tasksmack::Version::PROJECT_NAME, tasksmack::Version::STRING, tasksmack::Version::BUILD_TYPE);
    spdlog::debug("Compiler: {} {}", tasksmack::Version::COMPILER_ID, tasksmack::Version::COMPILER_VERSION);
    spdlog::debug("Built: {} {}", tasksmack::Version::BUILD_DATE, tasksmack::Version::BUILD_TIME);

    // Read TASKSMACK_SYNTHETIC once, now, so the synthetic scenario (#1413) is logged at startup and
    // every panel sees the same one. Unset, this is the only trace of it.
    static_cast<void>(App::Synthetic::activeScenario());

    // TASKSMACK_WINDOW (#1453), likewise read and logged once: a fixed geometry for measurement runs
    // that replaces the saved one here and is not saved on exit (ShellLayer::onDetach).
    const std::optional<App::WindowOverride::Geometry>& windowOverride = App::WindowOverride::active();

    if (instanceLock.status() == App::InstanceLock::Status::Unavailable)
    {
        spdlog::warn("Can't take the single-instance lock {}: {}; starting anyway", instanceLockPath.string(), instanceLock.error());
    }

    userConfig.load();
    const auto& settings = userConfig.settings();

    // Create application and transfer ownership to the singleton
    Core::ApplicationSpecification appSpec;
    appSpec.Name = "TaskSmack";
    appSpec.Width =
        std::clamp(windowOverride ? windowOverride->width : settings.windowWidth, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    appSpec.Height =
        std::clamp(windowOverride ? windowOverride->height : settings.windowHeight, Core::WINDOW_MIN_DIMENSION, Core::WINDOW_MAX_DIMENSION);
    appSpec.VSync = true;
    appSpec.ForceNativeDecorationsOnWayland = settings.forceNativeWindowDecorationsOnWayland;

    auto app = std::make_unique<Core::Application>(appSpec);
    Core::Application::setInstance(std::move(app));

    // Get reference to the application for further setup
    Core::Application& appRef = Core::Application::get();

    // From here the Application owns the layers. However the rest ends, detach them while the
    // Application singleton is still reachable, then destroy it, both before the config and theme
    // singletons are torn down at static destruction (#1124).
    const auto tearDownApplication = [&appRef]
    {
        // CRITICAL: Manually detach all layers BEFORE clearing the Application singleton.
        // Reason: Layer onDetach() methods may call Application::get() to save state.
        // If we let ~Application() run during setInstance(nullptr), those calls will fail.
        appRef.detachAllLayers();
        // Explicitly destroy the Application singleton to ensure SDL_Quit()
        // and other teardown happen before main()/WinMain() returns.
        Core::Application::setInstance(nullptr);
    };

    try
    {

        // Apply the saved normal geometry and maximized state now that the window exists. The size
        // and position are checked against the connected displays, so a position saved on a monitor
        // that is gone no longer opens the borderless window off-screen (#1128), and the normal
        // rectangle is applied before maximizing so it is what Restore returns to (#1121). The size is
        // converted from the scale it was saved at to the scale of the display it opens on (#1168).
        // A TASKSMACK_WINDOW override replaces all of it: its size (already given to the window
        // above) is kept in window units with no scale conversion, only fitted to the display, at
        // the position the window was created at, and maximized only if it says so.
        if (windowOverride)
        {
            appRef.getWindow().applySavedGeometry(std::nullopt, windowOverride->maximized, std::nullopt);
        }
        else
        {
            std::optional<std::pair<int, int>> savedPosition;
            if (settings.windowPosX.has_value() && settings.windowPosY.has_value())
            {
                savedPosition = std::pair{*settings.windowPosX, *settings.windowPosY};
            }
            appRef.getWindow().applySavedGeometry(savedPosition, settings.windowMaximized, settings.windowScale);
        }

        // Push UI layer (initializes ImGui/ImPlot backends). Must be pushed (and therefore
        // onRender()'d) before ShellLayer: UILayer::onRender() calls ImGui::NewFrame(), which is
        // what actually advances ImGui::GetFrameCount() -- ShellLayer::onRender() reads that count
        // to publish RenderMetrics' per-frame totals (see #875) and needs the *current* frame's
        // value, not the previous one.
        appRef.pushLayer<UI::UILayer>();

        // Push title bar layer (custom window chrome) -- skipped when native OS decorations are in
        // use instead (opt-in, native Wayland only; see #745), since the OS/compositor already draws
        // a title bar in that case and ShellLayer reserves no space for a second one.
        App::TitleBarLayer* titleBar = nullptr;
        if (appRef.getWindow().isBorderless())
        {
            titleBar = &appRef.pushLayer<App::TitleBarLayer>();
        }

        // Push shell layer (docking workspace with panels). It hands the title bar the width its
        // panels need, which the title bar folds into the window's minimum size (#1207).
        appRef.pushLayer<App::ShellLayer>().setTitleBar(titleBar);

        // The Help window (non-modal, #172) and the dialog layers (modal overlays), opened by
        // OpenHelpEvent, OpenAboutEvent, OpenSettingsEvent and OpenElevationNoticeEvent. The
        // elevation notice is shown at startup when running without elevated privileges.
        appRef.pushLayer<App::HelpLayer>();
        appRef.pushLayer<App::AboutLayer>();
        appRef.pushLayer<App::SettingsLayer>();
        appRef.pushLayer<App::ElevationNoticeLayer>();

        // Run the application
        appRef.run();
    }
    catch (...)
    {
        try
        {
            tearDownApplication();
        }
        catch (...) // NOLINT(bugprone-empty-catch) - already failing; report the original error
        {}
        throw;
    }

    tearDownApplication();
    return 0;
}

/// Runs the app, turning an exception that escapes it into a logged error and a non-zero exit
/// instead of std::terminate (#1124). On Windows there is no console, so it is also shown.
auto runAppGuarded() -> int
{
    try
    {
        return runApp();
    }
    catch (const std::exception& e)
    {
        try
        {
            spdlog::critical("TaskSmack stopped on an unexpected error: {}", e.what());
#ifdef _WIN32
            const std::string message = std::string("TaskSmack stopped on an unexpected error:\n\n") + e.what();
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TaskSmack", message.c_str(), nullptr);
#endif
        }
        catch (...) // NOLINT(bugprone-empty-catch) - reporting is best effort; the exit code still says it failed
        {}
        return EXIT_FAILURE;
    }
    catch (...)
    {
        // Something not derived from std::exception: same guarded exit, with a generic message.
        try
        {
            spdlog::critical("TaskSmack stopped on an unexpected error of an unknown type");
#ifdef _WIN32
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TaskSmack", "TaskSmack stopped on an unexpected error.", nullptr);
#endif
        }
        catch (...) // NOLINT(bugprone-empty-catch) - reporting is best effort; the exit code still says it failed
        {}
        return EXIT_FAILURE;
    }
}

} // namespace

// Entry points
#ifdef _WIN32
// Windows GUI application entry point
auto WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/) -> int
{
    return runAppGuarded();
}
#else
// Standard entry point for Linux/macOS
// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog initialization may theoretically throw; acceptable at program start
auto main() -> int
{
    return runAppGuarded();
}
#endif
