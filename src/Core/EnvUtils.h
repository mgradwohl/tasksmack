#pragma once

// The TASKSMACK_* environment hooks in one place (#2170): their names and what each does
// (KNOWN_ENV_HOOKS), how a value is trimmed (Platform::trimAscii(), the same for every hook), and the
// flag parsing several share. Reading them (readEnv(), with SDL and spdlog) is in Core/EnvRead.h, so
// this header stays dependency-free for the parsers that include it (and the fuzz builds of them). Each hook's own
// parser stays beside the code it configures; each of their name constants is checked against
// KNOWN_ENV_HOOKS at compile time (isKnownEnvHook()), so a new hook can't be left off the list.

#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace Core
{

/// One TASKSMACK_* environment hook: its name and what it does, in a line.
struct EnvHook
{
    std::string_view name;
    std::string_view description;
};

inline constexpr std::string_view TRACE_RESIZE_PERF_ENV_VAR = "TASKSMACK_TRACE_RESIZE_PERF";
inline constexpr std::string_view LOG_LEVEL_ENV_VAR = "TASKSMACK_LOG_LEVEL";

/// Every TASKSMACK_* variable TaskSmack reads at run time (the compile-time TASKSMACK_HAS_* macros
/// aren't hooks). Debugging and test aids, not settings: none is needed to run the app.
inline constexpr std::array KNOWN_ENV_HOOKS{
    EnvHook{.name = "TASKSMACK_CONFIG_DIR", .description = "Use this folder for config.toml and user themes instead of the usual one"},
    EnvHook{.name = "TASKSMACK_WINDOW", .description = "Open at WIDTHxHEIGHT, or WIDTHxHEIGHT,maximized; the geometry isn't saved"},
    EnvHook{.name = "TASKSMACK_SYNTHETIC", .description = "Show a generated machine (processes=N,cores=N,...) instead of this one"},
    EnvHook{.name = "TASKSMACK_SELECT_PID", .description = "Select this process at startup"},
    EnvHook{.name = "TASKSMACK_SELECT_NAME", .description = "Select the first process with this name at startup"},
    EnvHook{.name = "TASKSMACK_DETAILS_TAB", .description = "Open this Process Details tab for the selected process"},
    EnvHook{.name = "TASKSMACK_TAB", .description = "Start on this main tab"},
    EnvHook{.name = "TASKSMACK_MACHINE_TAB", .description = "Start on this tab of the machine view: overview, cores, gpu, npu or network"},
    EnvHook{.name = "TASKSMACK_OPEN", .description = "Open the help, about or settings dialog at startup"},
    EnvHook{.name = "TASKSMACK_SCREENSHOT",
            .description = "Write a PNG of the window (file[;after=N][;frames=N][;fps=N]) after startup, then quit"},
    EnvHook{.name = "TASKSMACK_DEMO",
            .description = "Replay timed UI requests (seconds:action=value,...) once startup has settled, for a capture"},
    EnvHook{.name = "TASKSMACK_LOCALE", .description = "Format numbers and dates with this locale instead of the user's"},
    EnvHook{.name = LOG_LEVEL_ENV_VAR, .description = "Log at this level: trace, debug, info, warn, error, critical or off"},
    EnvHook{.name = TRACE_RESIZE_PERF_ENV_VAR, .description = "Log resize and frame timings (and log at info)"},
};

/// Whether @p name is listed in KNOWN_ENV_HOOKS. Each hook's name constant is static_asserted with it.
[[nodiscard]] constexpr bool isKnownEnvHook(std::string_view name) noexcept
{
    return std::ranges::any_of(KNOWN_ENV_HOOKS, [name](const EnvHook& hook) { return hook.name == name; });
}

static_assert(isKnownEnvHook(TRACE_RESIZE_PERF_ENV_VAR) && isKnownEnvHook(LOG_LEVEL_ENV_VAR));

/// Returns true if the environment variable value represents a truthy flag.
/// "0", "false", "off", "no" (case-insensitive) and nullptr, empty or blank are treated
/// as disabled. Any other value (e.g. "1", "true", "yes", "on") is treated as enabled.
/// Surrounding ASCII whitespace is ignored, as for every hook (#2170).
[[nodiscard]] inline bool isEnvFlagEnabled(const char* value) noexcept
{
    if (value == nullptr)
    {
        return false;
    }

    const std::string_view text = Platform::trimAscii(value);
    if (text.empty())
    {
        return false;
    }

    using Platform::equalsIgnoreCase;
    return !equalsIgnoreCase(text, "0") && !equalsIgnoreCase(text, "false") && !equalsIgnoreCase(text, "off") &&
           !equalsIgnoreCase(text, "no");
}

} // namespace Core
