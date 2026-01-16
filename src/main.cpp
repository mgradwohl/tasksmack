#ifdef _WIN32
#include <windows.h>
#endif

#include "App/AboutLayer.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "UI/UILayer.h"
#include "version.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#ifdef _WIN32
#include <filesystem>
#include <locale>
#include <memory>
#include <vector>
#endif
#include <algorithm>
#include <clocale>
#include <exception>
#include <iostream>
#include <print>

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

// Required on Windows to see console output when launching from an IDE or debugger
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
    auto msvcSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    std::shared_ptr<spdlog::sinks::basic_file_sink_mt> fileSink;
    std::filesystem::path logPath;
    try
    {
        logPath = std::filesystem::temp_directory_path() / "tasksmack-debug.log";
        fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    }
    catch (const std::exception& e)
    {
        // Best-effort: if log file setup fails, still keep console + MSVC sinks.
        std::println(stderr, "Failed to initialize file logging: {}", e.what());
    }
    catch (...)
    {
        std::println(stderr, "Failed to initialize file logging (unknown error)");
    }

    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(3);
    sinks.push_back(msvcSink);
    sinks.push_back(consoleSink);
    if (fileSink)
    {
        sinks.push_back(fileSink);
    }

    auto logger = std::make_shared<spdlog::logger>("TaskSmack", sinks.begin(), sinks.end());

    spdlog::set_default_logger(logger);

    if (!logPath.empty())
    {
        spdlog::info("Debug log file: {}", logPath.string());
    }

#endif

#ifndef NDEBUG
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::debug);
#endif

    spdlog::info("{} v{} ({} build)", tasksmack::Version::PROJECT_NAME, tasksmack::Version::STRING, tasksmack::Version::BUILD_TYPE);
    spdlog::debug("Compiler: {} {}", tasksmack::Version::COMPILER_ID, tasksmack::Version::COMPILER_VERSION);
    spdlog::debug("Built: {} {}", tasksmack::Version::BUILD_DATE, tasksmack::Version::BUILD_TIME);

    // Load user configuration early so we can apply window geometry before creating the SDL window.
    auto& userConfig = App::UserConfig::get();
    userConfig.load();
    const auto& settings = userConfig.settings();

    // Create application and transfer ownership to the singleton
    Core::ApplicationSpecification appSpec;
    appSpec.Name = "TaskSmack";
    appSpec.Width = std::clamp(settings.windowWidth, 200, 16'384);
    appSpec.Height = std::clamp(settings.windowHeight, 200, 16'384);
    appSpec.VSync = true;

    auto app = std::make_unique<Core::Application>(appSpec);
    Core::Application::setInstance(std::move(app));

    // Get reference to the application for further setup
    Core::Application& appRef = Core::Application::get();

    // Apply saved position/maximized state after the window exists.
    // Ordering: set restore geometry first, then maximize.
    if (settings.windowPosX.has_value() && settings.windowPosY.has_value())
    {
        appRef.getWindow().setPosition(*settings.windowPosX, *settings.windowPosY);
    }
    if (settings.windowMaximized)
    {
        appRef.getWindow().maximize();
    }

    // Push UI layer (initializes ImGui/ImPlot backends)
    appRef.pushLayer<UI::UILayer>();

    // Push title bar layer (custom window chrome)
    appRef.pushLayer<App::TitleBarLayer>();

    // Push shell layer (docking workspace with panels)
    appRef.pushLayer<App::ShellLayer>();

    // About dialog layer (modal overlay)
    // NOTE: AboutLayer follows a singleton pattern exposed via App::AboutLayer::instance().
    // The layer stack owns the layer; setInstance() registers a non-owning reference.
    // pushLayer() calls onAttach(), which now allows s_Instance to be nullptr.
    auto& aboutLayerRef = appRef.pushLayer<App::AboutLayer>();
    App::AboutLayer::setInstance(aboutLayerRef);

    // Settings dialog layer (modal overlay)
    // NOTE: SettingsLayer follows the same singleton pattern as AboutLayer.
    auto& settingsLayerRef = appRef.pushLayer<App::SettingsLayer>();
    App::SettingsLayer::setInstance(settingsLayerRef);

    // Run the application
    appRef.run();

    // Explicitly destroy the Application singleton to ensure SDL_Quit()
    // and other teardown happen before main()/WinMain() returns.
    Core::Application::setInstance(nullptr);

    return 0;
}

} // namespace

// Entry points
#ifdef _WIN32
// Windows GUI application entry point
auto WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/) -> int
{
    return runApp();
}
#else
// Standard entry point for Linux/macOS
// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog initialization may theoretically throw; acceptable at program start
auto main() -> int
{
    return runApp();
}
#endif
