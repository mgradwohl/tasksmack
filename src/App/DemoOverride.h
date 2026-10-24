#pragma once

// A timed demo script (#2827): once startup has settled, replay a list of the requests the startup
// hooks and the UI make, each at its time, so TASKSMACK_SCREENSHOT can record an animation (a walk
// from the Processes table into Process Details, a theme switch) or a still of a view no startup
// hook reaches (a collapsed Process Details section) -- with no input at all.
//
//   TASKSMACK_DEMO="1:tab=processes,3:select=name:explorer.exe,5:details=network,8:theme=dracula"
//
// The value is read once, at startup (main() calls active()). It is a comma-separated list of
// <seconds>:<action>=<value> steps. The seconds (up to three decimals, not decreasing from step to
// step, at most MAX_STEP_SECONDS) count from the moment the startup selections (TASKSMACK_TAB,
// TASKSMACK_SELECT_PID/_NAME) have taken effect: the point ScreenshotLayer waits for too, so a
// screenshot sequence with after=0 records the script from its start. The actions:
//
//   tab=<name>                    the main tab, as TASKSMACK_TAB names one
//   select=name:<process name>    the process to select in the Processes table, as a click does
//   select=pid:<n>                (when it next appears in a snapshot); the tab does not change
//   details=<overview|gpu|npu|network>   Process Details brought forward, with that tab
//   machine=<overview|cores|gpu|npu|network>   the machine (hostname) tab brought forward, with
//                                 that sub-tab (as TASKSMACK_MACHINE_TAB names one, #2869)
//   open=<help|about>             the Help window or the About dialog
//   close=dialog                  closes them
//   theme=<id>                    a loaded theme, applied as Settings applies one
//   section=<name>                expands that section of Process Details' Overview (modules,
//                                 openfiles, connections, environment, security or services)
//
// A value that doesn't parse is ignored whole, with one warning naming the step. A tab or theme
// name is checked when its step runs (DemoLayer), since neither is known here. The parser is pure
// and header-only (tests/App/test_DemoOverride.cpp); only active() reads the environment and logs.

#include "App/SelectOverride.h"
#include "Core/EnvUtils.h"
#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace App::DemoOverride
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_DEMO";
static_assert(Core::isKnownEnvHook(ENV_VAR)); // listed in KNOWN_ENV_HOOKS (#2170)

/// A step's time at most, in seconds: an hour, longer than any capture (a TASKSMACK_SCREENSHOT
/// sequence runs at most MAX_FRAMES frames at 1 fps).
inline constexpr int MAX_STEP_SECONDS = 3600;
inline constexpr int MILLIS_PER_SECOND = 1000;
/// Decimals a time may have: milliseconds, finer than any frame.
inline constexpr std::size_t MAX_TIME_DECIMALS = 3;

/// tab=<name>: the main tab to select, resolved against the registered tabs when the step runs.
struct TabAction
{
    std::string name;

    [[nodiscard]] bool operator==(const TabAction&) const = default;
};

/// select=pid:<n> or select=name:<process name>: the process to select when it next appears.
struct SelectAction
{
    SelectOverride::Target target;

    [[nodiscard]] bool operator==(const SelectAction&) const = default;
};

/// details=<overview|gpu|npu|network>: Process Details brought forward, with that tab.
struct DetailsAction
{
    SelectOverride::DetailsTab tab = SelectOverride::DetailsTab::Overview;

    [[nodiscard]] bool operator==(const DetailsAction&) const = default;
};

/// machine=<overview|cores|gpu|npu|network>: the machine tab brought forward, with that sub-tab (#2869).
struct MachineAction
{
    SelectOverride::MachineTab tab = SelectOverride::MachineTab::Overview;

    [[nodiscard]] bool operator==(const MachineAction&) const = default;
};

/// open=<help|about>
struct OpenAction
{
    SelectOverride::StartupDialog dialog = SelectOverride::StartupDialog::Help;

    [[nodiscard]] bool operator==(const OpenAction&) const = default;
};

/// close=dialog: the Help window and the About dialog, whichever is open.
struct CloseDialogAction
{
    [[nodiscard]] bool operator==(const CloseDialogAction&) const = default;
};

/// theme=<id>: checked against the loaded themes when the step runs.
struct ThemeAction
{
    std::string id;

    [[nodiscard]] bool operator==(const ThemeAction&) const = default;
};

/// section=<name>: a collapsed section of Process Details' Overview, drawn open.
struct SectionAction
{
    SelectOverride::DetailsSection section = SelectOverride::DetailsSection::Modules;

    [[nodiscard]] bool operator==(const SectionAction&) const = default;
};

using Action =
    std::variant<TabAction, SelectAction, DetailsAction, MachineAction, OpenAction, CloseDialogAction, ThemeAction, SectionAction>;

/// One step: what to request, and when.
struct Step
{
    int timeMs = 0; ///< Milliseconds after the startup selections settled
    Action action;

    [[nodiscard]] bool operator==(const Step&) const = default;
};

struct ParseResult
{
    std::optional<std::vector<Step>> steps; ///< nullopt: no script (unset, blank, or invalid)
    std::string warning;                    ///< why the value was ignored; empty if it wasn't
};

namespace Detail
{

[[nodiscard]] inline bool allDigits(const std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; });
}

/// "<digits>[.<1 to 3 digits>]" as milliseconds: "1" is 1000, "0.5" 500, "2.25" 2250. The error
/// says what is wrong with anything else: a sign, a space, no digit before or after the point, more
/// than MAX_TIME_DECIMALS decimals, or a time past MAX_STEP_SECONDS.
[[nodiscard]] inline std::expected<int, std::string> parseTimeMs(const std::string_view text)
{
    const std::size_t point = text.find('.');
    const std::string_view whole = text.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view{} : text.substr(point + 1);
    if (!allDigits(whole) || (point != std::string_view::npos && (!allDigits(fraction) || fraction.size() > MAX_TIME_DECIMALS)))
    {
        return std::unexpected(
            std::format("has a time '{}' that isn't a number of seconds (digits, with up to {} decimals)", text, MAX_TIME_DECIMALS));
    }
    const auto pastTheLimit = [text]
    {
        return std::unexpected(std::format("has a time '{}' past {} s", text, MAX_STEP_SECONDS));
    };
    // Digits only, so the one error from_chars can report is a number too large for an int.
    int seconds = 0;
    const auto [end, error] = std::from_chars(whole.data(), whole.data() + whole.size(), seconds);
    if (error != std::errc{} || end != whole.data() + whole.size() || seconds > MAX_STEP_SECONDS)
    {
        return pastTheLimit();
    }
    int millis = 0;
    for (const char digit : fraction)
    {
        millis = (millis * 10) + (digit - '0');
    }
    for (std::size_t decimals = fraction.size(); decimals < MAX_TIME_DECIMALS; ++decimals)
    {
        millis *= 10;
    }
    const int timeMs = (seconds * MILLIS_PER_SECOND) + millis;
    if (timeMs > MAX_STEP_SECONDS * MILLIS_PER_SECOND)
    {
        return pastTheLimit();
    }
    return timeMs;
}

/// The action @p key names with @p value (both trimmed), or why it doesn't.
[[nodiscard]] inline std::expected<Action, std::string> parseAction(const std::string_view key, const std::string_view value)
{
    using Platform::equalsIgnoreCase;
    if (value.empty())
    {
        return std::unexpected(std::format("gives '{}' no value", key));
    }
    if (equalsIgnoreCase(key, "tab"))
    {
        return TabAction{.name = std::string(value)};
    }
    if (equalsIgnoreCase(key, "select"))
    {
        const std::size_t colon = value.find(':');
        if (colon == std::string_view::npos)
        {
            return std::unexpected(std::format("selects '{}', neither pid:<n> nor name:<process name>", value));
        }
        const std::string_view kind = Platform::trimAscii(value.substr(0, colon));
        const std::string_view what = Platform::trimAscii(value.substr(colon + 1));
        if (equalsIgnoreCase(kind, "pid"))
        {
            if (const std::optional<std::int32_t> pid = SelectOverride::parsePid(what); pid.has_value())
            {
                return SelectAction{.target = {.pid = pid, .name = {}, .tab = SelectOverride::DetailsTab::Overview}};
            }
            return std::unexpected(std::format("selects pid '{}', which isn't a positive PID", what));
        }
        if (equalsIgnoreCase(kind, "name"))
        {
            if (!what.empty())
            {
                return SelectAction{
                    .target = {.pid = std::nullopt, .name = std::string(what), .tab = SelectOverride::DetailsTab::Overview}};
            }
            return std::unexpected("selects an empty name");
        }
        return std::unexpected(std::format("selects '{}', neither pid:<n> nor name:<process name>", value));
    }
    if (equalsIgnoreCase(key, "details"))
    {
        if (const std::optional<SelectOverride::DetailsTab> tab = SelectOverride::parseTab(value); tab.has_value())
        {
            return DetailsAction{.tab = *tab};
        }
        return std::unexpected(std::format("names '{}', not overview, gpu, npu or network, as the details tab", value));
    }
    if (equalsIgnoreCase(key, "machine"))
    {
        if (const std::optional<SelectOverride::MachineTab> tab = SelectOverride::parseMachineTab(value); tab.has_value())
        {
            return MachineAction{.tab = *tab};
        }
        return std::unexpected(std::format("names '{}', not overview, cores, gpu, npu or network, as the machine tab", value));
    }
    if (equalsIgnoreCase(key, "open"))
    {
        if (const std::optional<SelectOverride::StartupDialog> dialog = SelectOverride::parseDialog(value); dialog.has_value())
        {
            return OpenAction{.dialog = *dialog};
        }
        return std::unexpected(std::format("opens '{}', not help or about", value));
    }
    if (equalsIgnoreCase(key, "close"))
    {
        if (equalsIgnoreCase(value, "dialog"))
        {
            return CloseDialogAction{};
        }
        return std::unexpected(std::format("closes '{}', not dialog", value));
    }
    if (equalsIgnoreCase(key, "theme"))
    {
        return ThemeAction{.id = std::string(value)};
    }
    if (equalsIgnoreCase(key, "section"))
    {
        if (const std::optional<SelectOverride::DetailsSection> section = SelectOverride::parseSection(value); section.has_value())
        {
            return SectionAction{.section = *section};
        }
        return std::unexpected(std::format("expands '{}', not modules, openfiles, connections, environment, security or services", value));
    }
    return std::unexpected(std::format("has an unknown action '{}'", key));
}

/// One "<seconds>:<action>=<value>" step (trimmed), or why it isn't one.
[[nodiscard]] inline std::expected<Step, std::string> parseStep(const std::string_view stepText)
{
    if (stepText.empty())
    {
        return std::unexpected("is empty");
    }
    const std::size_t colon = stepText.find(':');
    if (colon == std::string_view::npos)
    {
        return std::unexpected("has no ':' between its seconds and its action");
    }
    const std::expected<int, std::string> timeMs = parseTimeMs(Platform::trimAscii(stepText.substr(0, colon)));
    if (!timeMs.has_value())
    {
        return std::unexpected(timeMs.error());
    }
    const std::string_view actionText = Platform::trimAscii(stepText.substr(colon + 1));
    const std::size_t equals = actionText.find('=');
    if (equals == std::string_view::npos)
    {
        return std::unexpected("has no '=' between its action and the value");
    }
    std::expected<Action, std::string> action =
        parseAction(Platform::trimAscii(actionText.substr(0, equals)), Platform::trimAscii(actionText.substr(equals + 1)));
    if (!action.has_value())
    {
        return std::unexpected(action.error());
    }
    return Step{.timeMs = *timeMs, .action = std::move(*action)};
}

/// A step's action as the script spells it: "tab=processes", "select=pid:42", "close=dialog".
struct Describe
{
    [[nodiscard]] std::string operator()(const TabAction& action) const
    {
        return "tab=" + action.name;
    }
    [[nodiscard]] std::string operator()(const SelectAction& action) const
    {
        return action.target.pid.has_value() ? std::format("select=pid:{}", *action.target.pid) : "select=name:" + action.target.name;
    }
    [[nodiscard]] std::string operator()(const DetailsAction& action) const
    {
        return std::format("details={}", SelectOverride::tabName(action.tab));
    }
    [[nodiscard]] std::string operator()(const MachineAction& action) const
    {
        return std::format("machine={}", SelectOverride::machineTabName(action.tab));
    }
    [[nodiscard]] std::string operator()(const OpenAction& action) const
    {
        return std::format("open={}", SelectOverride::dialogName(action.dialog));
    }
    [[nodiscard]] std::string operator()(const CloseDialogAction& /*action*/) const
    {
        return "close=dialog";
    }
    [[nodiscard]] std::string operator()(const ThemeAction& action) const
    {
        return "theme=" + action.id;
    }
    [[nodiscard]] std::string operator()(const SectionAction& action) const
    {
        return std::format("section={}", SelectOverride::sectionName(action.section));
    }
};

} // namespace Detail

/// Parses a TASKSMACK_DEMO value: comma-separated "<seconds>:<action>=<value>" steps, spaces around
/// each part allowed, the action and the value's keywords in any case. nullptr, empty and blank give
/// no script and no warning. Anything else that isn't a script -- an empty step, a step without ':'
/// or '=', a time that isn't digits with up to three decimals, is past MAX_STEP_SECONDS or is
/// earlier than the step before it, an unknown action, or a value the action can't take -- gives no
/// script and a warning naming the step.
[[nodiscard]] inline ParseResult parse(const char* value)
{
    ParseResult result;
    const std::string_view text = Platform::trimAscii(value != nullptr ? value : "");
    if (text.empty())
    {
        return result;
    }

    std::vector<Step> steps;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t end = text.find(',', start);
        const std::string_view stepText = Platform::trimAscii(text.substr(start, end - start));
        std::expected<Step, std::string> step = Detail::parseStep(stepText);
        if (step.has_value() && !steps.empty() && step->timeMs < steps.back().timeMs)
        {
            step = std::unexpected("is earlier than the step before it");
        }
        if (!step.has_value())
        {
            result.warning = std::format("'{}': step {} ('{}') {}; ignored (expected <seconds>:<action>=<value> steps, comma-separated, "
                                         "their times not decreasing)",
                                         text,
                                         steps.size() + 1,
                                         stepText,
                                         step.error());
            return result;
        }
        steps.push_back(std::move(*step));
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    result.steps = std::move(steps);
    return result;
}

/// @p timeMs in seconds, as the script would write it: 1000 is "1", 1500 "1.5", 2250 "2.25", 1 "0.001".
[[nodiscard]] inline std::string formatSeconds(const int timeMs)
{
    std::string text = std::format("{}", timeMs / MILLIS_PER_SECOND);
    if (const int millis = timeMs % MILLIS_PER_SECOND; millis != 0)
    {
        std::string fraction = std::format(".{:03}", millis);
        while (fraction.ends_with('0'))
        {
            fraction.pop_back();
        }
        text += fraction;
    }
    return text;
}

/// "3:select=name:explorer.exe", as the script spells it.
[[nodiscard]] inline std::string describe(const Step& step)
{
    return std::format("{}:{}", formatSeconds(step.timeMs), std::visit(Detail::Describe{}, step.action));
}

/// "1:tab=processes, 3:select=name:explorer.exe": every step, comma-separated.
[[nodiscard]] inline std::string describe(const std::vector<Step>& steps)
{
    std::string text;
    for (const Step& step : steps)
    {
        text += (text.empty() ? "" : ", ") + describe(step);
    }
    return text;
}

/// The script TASKSMACK_DEMO gives, read and logged on the first call (main() makes it at startup);
/// nullopt when it is unset or invalid. Thread-safe.
[[nodiscard]] const std::optional<std::vector<Step>>& active();

} // namespace App::DemoOverride
