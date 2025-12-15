#ifdef _WIN32
#include <windows.h>
#endif

#include "version.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#ifdef _WIN32
#include <memory>
#endif
#include <cstdio>

auto main() -> int
{
// Required on Windows to see console output when launching from an IDE or debugger
#if defined(_WIN32) && !defined(NDEBUG)
    // Try to attach to parent console if it's a console app
    // If no parent console exists because it's a Windows app create our own console
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
    {
        AllocConsole();
        // Redirect stdout/stderr to the new console
        FILE* out = nullptr; // NOLINT(misc-const-correctness)
        FILE* err = nullptr; // NOLINT(misc-const-correctness)
        freopen_s(&out, "CONOUT$", "w", stdout);
        freopen_s(&err, "CONOUT$", "w", stderr);
    }
    auto msvcSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("MyProject", spdlog::sinks_init_list{msvcSink, consoleSink});

    spdlog::set_default_logger(logger);

#endif

#ifndef NDEBUG
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::debug);
#endif

    spdlog::info("{} v{} ({} build)", myproject::Version::PROJECT_NAME, myproject::Version::STRING, myproject::Version::BUILD_TYPE);
    spdlog::debug("Compiler: {} {}", myproject::Version::COMPILER_ID, myproject::Version::COMPILER_VERSION);
    spdlog::debug("Built: {} {}", myproject::Version::BUILD_DATE, myproject::Version::BUILD_TIME);

    std::printf("Hello, World!\n");
    return 0;
}