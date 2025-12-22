#ifdef _WIN32
#include <windows.h>
#endif

#include "App/AboutLayer.h"
#include "App/ShellLayer.h"
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
#include <memory>
#include <vector>
#endif
#include <cstdio>
#include <print>

namespace
{
auto runApp() -> int
{
// Required on Windows to see console output when launching from an IDE or debugger
#if defined(_WIN32) && !defined(NDEBUG)
    // Try to attach to parent console if it's a console app
    // If no parent console exists because it's a Windows app create our own console
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
    {
        AllocConsole();
        // Redirect stdout/stderr to the new console
        FILE* out = nullptr;
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

    // Load user configuration early so we can apply window geometry before creating the GLFW window.
    auto& userConfig = App::UserConfig::get();
    userConfig.load();
    const auto& settings = userConfig.settings();

    // Create application
    Core::ApplicationSpecification appSpec;
    appSpec.Name = "TaskSmack";
    appSpec.Width = std::clamp(settings.windowWidth, 200, 16'384);
    appSpec.Height = std::clamp(settings.windowHeight, 200, 16'384);
    appSpec.VSync = true;

    Core::Application app(appSpec);

    // Apply saved position/maximized state after the window exists.
    // Ordering: set restore geometry first, then maximize.
    if (settings.windowPosX.has_value() && settings.windowPosY.has_value())
    {
        app.getWindow().setPosition(*settings.windowPosX, *settings.windowPosY);
    }
    if (settings.windowMaximized)
    {
        app.getWindow().maximize();
    }

    // Push UI layer (initializes ImGui/ImPlot backends)
    app.pushLayer<UI::UILayer>();

    // Push shell layer (docking workspace with panels)
    app.pushLayer<App::ShellLayer>();

    // About dialog layer (modal overlay)
    app.pushLayer<App::AboutLayer>();

    // Run the application
    app.run();

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
auto main() -> int
{
    return runApp();
}
#endif
