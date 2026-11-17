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

namespace Detail
{
/// Whether the hooks that write files are ignored (ignoreWritingHooks()). Process-wide, set once at startup.
[[nodiscard]] inline bool& writingHooksIgnoredFlag() noexcept
{
    static bool ignored = false;
    return ignored;
}
} // namespace Detail

/// The environment variable @p name as it is, whatever ignoreWritingHooks() says: for saying that a hook is set but
/// ignored, never for acting on its value.
[[nodiscard]] inline const char* readEnvUnfiltered(std::string_view name)
{
    return SDL_getenv(std::string(name).c_str());
}

/// From now on, a hook that writes a file or folder at a path it names (hookWritesFiles(): TASKSMACK_CONFIG_DIR,
/// TASKSMACK_SCREENSHOT, TASKSMACK_DEMO, TASKSMACK_RENDER_METRICS) reads as unset (#4104). main() calls it first thing
/// in a session whose environment may be the less-privileged side's: Windows elevated or restarted with --handoff, Linux
/// run as root for a user (sudo -E, an env_keep list). Such a session writes nothing where that side chose. The one gate
/// for every writing hook: the config folder's ignoreTestHook() (#3920, #4090) is this. Not thread-safe: startup only.
inline void ignoreWritingHooks() noexcept
{
    Detail::writingHooksIgnoredFlag() = true;
}

/// Whether ignoreWritingHooks() was called.
[[nodiscard]] inline bool writingHooksIgnored() noexcept
{
    return Detail::writingHooksIgnoredFlag();
}

/// The environment variable @p name, or nullptr when unset, or when it is a hook that writes files and
/// ignoreWritingHooks() was called (#4104). SDL_getenv(): UTF-8 on Windows too, no CRT deprecation, and it needs no
/// SDL_Init.
[[nodiscard]] inline const char* readEnv(std::string_view name)
{
    if (writingHooksIgnored() && hookWritesFiles(name))
    {
        return nullptr;
    }
    return readEnvUnfiltered(name);
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
