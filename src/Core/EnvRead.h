#pragma once

// Reading the TASKSMACK_* hooks (#2170): the one SDL_getenv() call, and the reads two hooks share. Apart
// from Core/EnvUtils.h (the list and the pure parsing), which parser headers include, so a parser built
// without SDL or spdlog (the fuzz targets) doesn't need them.

#include "Core/EnvUtils.h"
#include "Platform/AsciiTrim.h"

#include <SDL3/SDL_stdinc.h>
#include <spdlog/common.h>

#include <string>
#include <string_view>

namespace Core
{

/// The environment variable @p name, or nullptr when unset. SDL_getenv(): UTF-8 on Windows too, no CRT
/// deprecation, and it needs no SDL_Init.
[[nodiscard]] inline const char* readEnv(std::string_view name)
{
    return SDL_getenv(std::string(name).c_str());
}

/// Whether TASKSMACK_TRACE_RESIZE_PERF asks for resize tracing. Read on each call, not once: the
/// tests set and clear it around the Application they construct.
[[nodiscard]] inline bool resizePerfTraceRequested()
{
    return isEnvFlagEnabled(readEnv(TRACE_RESIZE_PERF_ENV_VAR));
}

/// The log level a TASKSMACK_LOG_LEVEL value names (spdlog's names), surrounding whitespace ignored.
/// spdlog maps an unknown name to off.
[[nodiscard]] inline spdlog::level::level_enum logLevelFromEnv(const char* value)
{
    return spdlog::level::from_str(std::string(Platform::trimAscii(value != nullptr ? value : "")));
}

} // namespace Core
