/// @file test_EnvHooks.cpp
/// @brief Every TASKSMACK_* hook in Core::KNOWN_ENV_HOOKS (#2170): each parses a value with ASCII
/// whitespace around it (" \t\v" before, "\r\n" after, as from a Windows .env file) as it parses the
/// value alone. The table must have a row per listed hook, so a new hook gets one.

#include "App/DemoOverride.h"
#include "App/RenderMetricsOverride.h"
#include "App/ScreenshotOverride.h"
#include "App/SelectOverride.h"
#include "App/SyntheticScenario.h"
#include "App/WindowOverride.h"
#include "Core/ConfigDirOverride.h"
#include "Core/EnvRead.h"
#include "Core/EnvUtils.h"
#include "Core/LocaleSetup.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace
{

/// A hook's parser, reduced to a string that says everything it decided about @p value.
struct HookParser
{
    std::string_view name;
    const char* value; ///< A valid value, unpadded
    std::function<std::string(const char*)> parse;
};

[[nodiscard]] std::string selection(const App::SelectOverride::ParseResult& result)
{
    if (!result.target)
    {
        return std::format("none, {} warnings", result.warnings.size());
    }
    return std::format("pid {} name '{}' tab {}, {} warnings",
                       result.target->pid.value_or(-1),
                       result.target->name,
                       static_cast<int>(result.target->tab),
                       result.warnings.size());
}

const std::array<HookParser, 15>& hookParsers()
{
    static const std::array<HookParser, 15> parsers{
        HookParser{.name = "TASKSMACK_CONFIG_DIR",
                   .value = "/tmp/tasksmack config",
                   .parse =
                       [](const char* value)
                   {
                       const auto dir = Core::ConfigDirOverride::parse(value);
                       return dir ? dir->generic_string() : std::string("none");
                   }},
        HookParser{.name = "TASKSMACK_WINDOW",
                   .value = "1280x720,maximized",
                   .parse =
                       [](const char* value)
                   {
                       const auto parsed = App::WindowOverride::parse(value);
                       return (parsed.geometry ? App::WindowOverride::describe(*parsed.geometry) : std::string("none")) + "|" +
                              parsed.warning;
                   }},
        HookParser{.name = "TASKSMACK_SYNTHETIC",
                   .value = "processes=10,refresh=500",
                   .parse =
                       [](const char* value)
                   {
                       const auto parsed = App::Synthetic::parseScenario(value);
                       return std::format(
                           "{}|{}", parsed.config ? App::Synthetic::describe(*parsed.config) : std::string("none"), parsed.warnings.size());
                   }},
        HookParser{.name = "TASKSMACK_SELECT_PID",
                   .value = "1234",
                   .parse = [](const char* value) { return selection(App::SelectOverride::parse(value, nullptr, nullptr)); }},
        HookParser{.name = "TASKSMACK_SELECT_NAME",
                   .value = "bash",
                   .parse = [](const char* value) { return selection(App::SelectOverride::parse(nullptr, value, nullptr)); }},
        HookParser{.name = "TASKSMACK_DETAILS_TAB",
                   .value = "network",
                   .parse = [](const char* value) { return selection(App::SelectOverride::parse("1234", nullptr, value)); }},
        HookParser{.name = "TASKSMACK_TAB",
                   .value = "processes",
                   .parse =
                       [](const char* value)
                   {
                       static constexpr std::array<App::SelectOverride::TabInfo, 2> TABS{
                           App::SelectOverride::TabInfo{.id = "processes", .text = "Processes"},
                           App::SelectOverride::TabInfo{.id = "services", .text = "Services"}};
                       const auto choice = App::SelectOverride::resolveMainTab(value, TABS);
                       return choice.id.value_or("none") + "|" + choice.warning;
                   }},
        HookParser{.name = "TASKSMACK_MACHINE_TAB",
                   .value = "cores",
                   .parse =
                       [](const char* value)
                   {
                       const auto choice = App::SelectOverride::parseStartupMachineTab(value);
                       return std::format("{}|{}", choice.tab ? static_cast<int>(*choice.tab) : -1, choice.warning);
                   }},
        HookParser{.name = "TASKSMACK_OPEN",
                   .value = "about",
                   .parse =
                       [](const char* value)
                   {
                       const auto choice = App::SelectOverride::parseStartupDialog(value);
                       return std::format("{}|{}", choice.dialog ? static_cast<int>(*choice.dialog) : -1, choice.warning);
                   }},
        HookParser{.name = "TASKSMACK_SCREENSHOT",
                   .value = "shot.png;after=2;frames=3;fps=5",
                   .parse =
                       [](const char* value)
                   {
                       const auto parsed = App::ScreenshotOverride::parse(value);
                       return (parsed.request ? App::ScreenshotOverride::describe(*parsed.request) : std::string("none")) + "|" +
                              parsed.warning;
                   }},
        HookParser{.name = "TASKSMACK_DEMO",
                   .value = "1:tab=processes,2.5:theme=dracula",
                   .parse =
                       [](const char* value)
                   {
                       const auto parsed = App::DemoOverride::parse(value);
                       return (parsed.steps ? App::DemoOverride::describe(*parsed.steps) : std::string("none")) + "|" + parsed.warning;
                   }},
        HookParser{.name = "TASKSMACK_RENDER_METRICS",
                   .value = "perf-data/idle metrics.csv",
                   .parse =
                       [](const char* value)
                   {
                       const auto path = App::RenderMetricsOverride::parse(value);
                       return path ? path->generic_string() : std::string("none");
                   }},
        HookParser{.name = "TASKSMACK_LOCALE",
                   .value = "de_DE",
                   .parse =
                       [](const char* value)
                   {
                       std::string joined;
                       for (const std::string& name : Core::LocaleSetup::localeOverrideCandidates(value))
                       {
                           joined += name + ";";
                       }
                       return joined;
                   }},
        HookParser{.name = Core::LOG_LEVEL_ENV_VAR,
                   .value = "debug",
                   .parse = [](const char* value) { return std::to_string(static_cast<int>(Core::logLevelFromEnv(value))); }},
        HookParser{.name = Core::TRACE_RESIZE_PERF_ENV_VAR,
                   .value = "0", // a disabled flag: padded, it read as enabled before #2170
                   .parse = [](const char* value) { return std::string(Core::isEnvFlagEnabled(value) ? "on" : "off"); }},
    };
    return parsers;
}

TEST(EnvHooksTest, EveryListedHookHasAParserRowAndEveryRowIsListed)
{
    for (const Core::EnvHook& hook : Core::KNOWN_ENV_HOOKS)
    {
        EXPECT_TRUE(std::ranges::any_of(hookParsers(), [&hook](const HookParser& row) { return row.name == hook.name; }))
            << hook.name << " has no row here";
        EXPECT_FALSE(hook.description.empty()) << hook.name;
    }
    for (const HookParser& row : hookParsers())
    {
        EXPECT_TRUE(Core::isKnownEnvHook(row.name)) << row.name << " isn't in KNOWN_ENV_HOOKS";
    }
}

TEST(EnvHooksTest, EveryHookIgnoresAsciiWhitespaceAroundItsValue)
{
    for (const HookParser& row : hookParsers())
    {
        SCOPED_TRACE(row.name);
        const std::string plain = row.parse(row.value);
        const std::string padded = row.parse((std::string(" \t\v") + row.value + "\r\n").c_str());
        EXPECT_EQ(padded, plain);
    }
}

TEST(EnvHooksTest, ABlankFlagIsOff)
{
    EXPECT_FALSE(Core::isEnvFlagEnabled(" \t\r\n"));
    EXPECT_TRUE(Core::isEnvFlagEnabled(" 1\r\n"));
    EXPECT_FALSE(Core::isEnvFlagEnabled(" off\r\n"));
}

} // namespace
