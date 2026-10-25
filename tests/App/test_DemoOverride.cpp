/// @file test_DemoOverride.cpp
/// @brief App::DemoOverride (#2827): the TASKSMACK_DEMO parser -- the steps' times, their order, each
/// action's value, and the scripts it refuses -- and how a script is described back.

#include "App/DemoOverride.h"
#include "App/SelectOverride.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using App::DemoOverride::Action;
using App::DemoOverride::CloseDialogAction;
using App::DemoOverride::describe;
using App::DemoOverride::DetailsAction;
using App::DemoOverride::formatSeconds;
using App::DemoOverride::MachineAction;
using App::DemoOverride::MAX_STEP_SECONDS;
using App::DemoOverride::OpenAction;
using App::DemoOverride::parse;
using App::DemoOverride::SectionAction;
using App::DemoOverride::SelectAction;
using App::DemoOverride::Step;
using App::DemoOverride::TabAction;
using App::DemoOverride::ThemeAction;
using App::SelectOverride::DetailsSection;
using App::SelectOverride::DetailsTab;
using App::SelectOverride::MachineTab;
using App::SelectOverride::StartupDialog;

[[nodiscard]] Step step(const int timeMs, Action action)
{
    return Step{.timeMs = timeMs, .action = std::move(action)};
}

[[nodiscard]] SelectAction byName(std::string name)
{
    return SelectAction{.target = {.pid = std::nullopt, .name = std::move(name), .tab = DetailsTab::Overview}};
}

[[nodiscard]] SelectAction byPid(const std::int32_t pid)
{
    return SelectAction{.target = {.pid = pid, .name = {}, .tab = DetailsTab::Overview}};
}

/// What @p value parses to, as describe() spells it, or "none: <warning>".
[[nodiscard]] std::string parsed(const char* value)
{
    const auto result = parse(value);
    return result.steps.has_value() ? describe(*result.steps) : "none: " + result.warning;
}

// ========== Parser ==========

TEST(DemoOverrideParseTest, UnsetAndBlankGiveNoScriptAndNoWarning)
{
    for (const char* value : {static_cast<const char*>(nullptr), "", "   ", "\t\r\n"})
    {
        SCOPED_TRACE(value == nullptr ? "(nullptr)" : value);
        const auto result = parse(value);
        EXPECT_FALSE(result.steps.has_value());
        EXPECT_TRUE(result.warning.empty()) << result.warning;
    }
}

TEST(DemoOverrideParseTest, ParsesEveryActionAndKeepsTheStepsInOrder)
{
    const auto result = parse("1:tab=processes,3:select=name:explorer.exe,3:select=pid:42,5:details=network,6:open=help,"
                              "7:close=dialog,8:theme=dracula,9:section=modules,10:machine=cores");
    ASSERT_TRUE(result.steps.has_value()) << result.warning;
    EXPECT_TRUE(result.warning.empty()) << result.warning;
    const std::vector<Step> steps = result.steps.value_or(std::vector<Step>{});
    const std::vector<Step> expected{
        step(1000, TabAction{.name = "processes"}),
        step(3000, byName("explorer.exe")),
        step(3000, byPid(42)),
        step(5000, DetailsAction{.tab = DetailsTab::Network}),
        step(6000, OpenAction{.dialog = StartupDialog::Help}),
        step(7000, CloseDialogAction{}),
        step(8000, ThemeAction{.id = "dracula"}),
        step(9000, SectionAction{.section = DetailsSection::Modules}),
        step(10000, MachineAction{.tab = MachineTab::Cores}),
    };
    EXPECT_EQ(steps, expected);
    EXPECT_EQ(describe(steps),
              "1:tab=processes, 3:select=name:explorer.exe, 3:select=pid:42, 5:details=network, 6:open=help, 7:close=dialog, "
              "8:theme=dracula, 9:section=modules, 10:machine=cores");
}

TEST(DemoOverrideParseTest, KeywordsParseInAnyCaseWithSpacesAroundEveryPart)
{
    // The action, the select kind and each keyword value in any case; a tab name and a theme id are
    // kept as given, since they are resolved when the step runs.
    EXPECT_EQ(parsed(" 1 : TAB = Processes , 2 : Select = Name : Explorer.EXE ,2:SELECT=PID: 7 , 3 : Details = GPU , 4:Open=ABOUT, "
                     "4.5:OPEN=Settings, 5:Close=DIALOG, 6:Theme=Dracula, 7:Section=OpenFiles, 8 : Machine = GPU , 9:MACHINE=Network"),
              "1:tab=Processes, 2:select=name:Explorer.EXE, 2:select=pid:7, 3:details=gpu, 4:open=about, 4.5:open=settings, "
              "5:close=dialog, 6:theme=Dracula, 7:section=openfiles, 8:machine=gpu, 9:machine=network");
}

TEST(DemoOverrideParseTest, EveryDetailsTabDialogAndSectionIsAccepted)
{
    EXPECT_EQ(parsed("0:details=overview,0:details=gpu,0:details=npu,0:details=network"),
              "0:details=overview, 0:details=gpu, 0:details=npu, 0:details=network");
    // #2869: the machine tab's sub-tabs, TASKSMACK_MACHINE_TAB's names.
    EXPECT_EQ(parsed("0:machine=overview,0:machine=cores,0:machine=gpu,0:machine=npu,0:machine=network"),
              "0:machine=overview, 0:machine=cores, 0:machine=gpu, 0:machine=npu, 0:machine=network");
    EXPECT_EQ(parsed("0:open=help,0:open=about,0:open=settings"), "0:open=help, 0:open=about, 0:open=settings");
    EXPECT_EQ(parsed("0:section=modules,0:section=openfiles,0:section=connections,0:section=environment,0:section=security,"
                     "0:section=services"),
              "0:section=modules, 0:section=openfiles, 0:section=connections, 0:section=environment, 0:section=security, "
              "0:section=services");
}

TEST(DemoOverrideParseTest, TimesAreSecondsWithUpToThreeDecimals)
{
    struct Case
    {
        const char* time = nullptr;
        int timeMs = 0;
    };
    for (const Case& c : {
             Case{.time = "0", .timeMs = 0},
             Case{.time = "1", .timeMs = 1000},
             Case{.time = "007", .timeMs = 7000},
             Case{.time = "0.5", .timeMs = 500},
             Case{.time = "2.25", .timeMs = 2250},
             Case{.time = "3.125", .timeMs = 3125},
             Case{.time = "0.001", .timeMs = 1},
             Case{.time = "0.10", .timeMs = 100},
             Case{.time = "3600", .timeMs = MAX_STEP_SECONDS * 1000},
         })
    {
        SCOPED_TRACE(c.time);
        const auto result = parse((std::string(c.time) + ":close=dialog").c_str());
        ASSERT_TRUE(result.steps.has_value()) << result.warning;
        const std::vector<Step> steps = result.steps.value_or(std::vector<Step>{});
        ASSERT_EQ(steps.size(), 1U);
        EXPECT_EQ(steps.front().timeMs, c.timeMs);
    }
    EXPECT_EQ(MAX_STEP_SECONDS, 3600);
}

TEST(DemoOverrideParseTest, TimesMayRepeatButNotGoBack)
{
    EXPECT_EQ(parsed("2:close=dialog,2:open=help"), "2:close=dialog, 2:open=help");
    const auto result = parse("2:close=dialog,1.999:open=help");
    EXPECT_FALSE(result.steps.has_value());
    EXPECT_NE(result.warning.find("step 2"), std::string::npos) << result.warning;
    EXPECT_NE(result.warning.find("earlier than the step before it"), std::string::npos) << result.warning;
}

TEST(DemoOverrideParseTest, InvalidScriptsAreIgnoredWholeWithAWarningNamingTheStep)
{
    struct Case
    {
        const char* value = nullptr;
        const char* step = nullptr; ///< the step the warning names
    };
    for (const Case& c : {
             // The step's shape
             Case{.value = "tab=processes", .step = "step 1"},
             Case{.value = "1 tab=processes", .step = "step 1"},
             Case{.value = "1:tab", .step = "step 1"},
             Case{.value = "1:tab=", .step = "step 1"},
             Case{.value = "1:=processes", .step = "step 1"},
             Case{.value = "1:tab=processes,", .step = "step 2"},
             Case{.value = ",1:tab=processes", .step = "step 1"},
             Case{.value = "1:tab=a,,2:tab=b", .step = "step 2"},
             Case{.value = "1:tab=a,2:bogus=1", .step = "step 2"},
             // The time
             Case{.value = "x:tab=a", .step = "step 1"},
             Case{.value = "-1:tab=a", .step = "step 1"},
             Case{.value = "+1:tab=a", .step = "step 1"},
             Case{.value = "1.:tab=a", .step = "step 1"},
             Case{.value = ".5:tab=a", .step = "step 1"},
             Case{.value = "1.2345:tab=a", .step = "step 1"},
             Case{.value = "1 0:tab=a", .step = "step 1"},
             Case{.value = "3600.001:tab=a", .step = "step 1"},
             Case{.value = "3601:tab=a", .step = "step 1"},
             Case{.value = "99999999999999999999:tab=a", .step = "step 1"},
             // The values
             Case{.value = "1:select=explorer.exe", .step = "step 1"},
             Case{.value = "1:select=pid:0", .step = "step 1"},
             Case{.value = "1:select=pid:abc", .step = "step 1"},
             Case{.value = "1:select=pid:", .step = "step 1"},
             Case{.value = "1:select=name:", .step = "step 1"},
             Case{.value = "1:select=user:bob", .step = "step 1"},
             Case{.value = "1:details=memory", .step = "step 1"},
             Case{.value = "1:machine=memory", .step = "step 1"},
             Case{.value = "1:machine=cpu cores", .step = "step 1"},
             Case{.value = "1:machine=", .step = "step 1"},
             Case{.value = "1:open=preferences", .step = "step 1"},
             Case{.value = "1:close=window", .step = "step 1"},
             Case{.value = "1:section=files", .step = "step 1"},
             Case{.value = "1:jump=processes", .step = "step 1"},
         })
    {
        SCOPED_TRACE(c.value);
        const auto result = parse(c.value);
        EXPECT_FALSE(result.steps.has_value());
        EXPECT_NE(result.warning.find(c.step), std::string::npos) << result.warning;
        EXPECT_NE(result.warning.find("ignored"), std::string::npos) << result.warning;
    }
}

TEST(DemoOverrideParseTest, TheWarningSaysWhatIsWrong)
{
    EXPECT_NE(parse("1 tab=a").warning.find("has no ':'"), std::string::npos);
    EXPECT_NE(parse("1:tab").warning.find("has no '='"), std::string::npos);
    EXPECT_NE(parse("1:tab=").warning.find("gives 'tab' no value"), std::string::npos);
    EXPECT_NE(parse("x:tab=a").warning.find("isn't a number of seconds"), std::string::npos);
    EXPECT_NE(parse("3601:tab=a").warning.find("past 3600 s"), std::string::npos);
    EXPECT_NE(parse("1:select=pid:x").warning.find("isn't a positive PID"), std::string::npos);
    EXPECT_NE(parse("1:details=memory").warning.find("not overview, gpu, npu or network"), std::string::npos);
    EXPECT_NE(parse("1:machine=memory").warning.find("not overview, cores, gpu, npu or network, as the machine tab"), std::string::npos);
    EXPECT_NE(parse("1:open=x").warning.find("not help, about or settings"), std::string::npos);
    EXPECT_NE(parse("1:close=x").warning.find("not dialog"), std::string::npos);
    EXPECT_NE(parse("1:section=x").warning.find("not modules, openfiles, connections, environment, security or services"),
              std::string::npos);
    EXPECT_NE(parse("1:jump=x").warning.find("unknown action 'jump'"), std::string::npos);
    EXPECT_NE(parse("1:tab=a,").warning.find("is empty"), std::string::npos);
}

// A process name or a tab name may hold ':', '=' and ';' (the first ':' ends the seconds, the first
// '=' the action); only ',' separates steps.
TEST(DemoOverrideParseTest, ValuesKeepTheirOwnColonsEqualsAndSemicolons)
{
    EXPECT_EQ(parsed("1:select=name:a:b=c"), "1:select=name:a:b=c");
    EXPECT_EQ(parsed("1:tab=x=y:z"), "1:tab=x=y:z");
    EXPECT_EQ(parsed("1:theme=my=theme"), "1:theme=my=theme");
    EXPECT_EQ(parsed("1:tab=a;2:tab=b"), "1:tab=a;2:tab=b"); // one step: ';' is not the separator
}

// #2170: every TASKSMACK_* hook trims the same ASCII whitespace (Platform::trimAscii()), so a value
// copied from a Windows .env file, with its trailing CR, parses as it does without one.
TEST(DemoOverrideParseTest, AnyAsciiWhitespaceAroundTheValueIsTrimmed)
{
    EXPECT_EQ(parsed("\t1:tab=processes, 2:theme=dracula\r\n"), "1:tab=processes, 2:theme=dracula");
}

// The value is UTF-8, as every hook's is (#1648): a process name is kept byte for byte.
TEST(DemoOverrideParseTest, NamesAreKeptAsUtf8)
{
    const auto result = parse("1:select=name:процесс.exe");
    ASSERT_TRUE(result.steps.has_value()) << result.warning;
    const std::vector<Step> steps = result.steps.value_or(std::vector<Step>{});
    ASSERT_EQ(steps.size(), 1U);
    EXPECT_EQ(steps.front().action, Action{byName("процесс.exe")});
}

// ========== Describing a script ==========

TEST(DemoOverrideTest, FormatSecondsWritesWholeSecondsAndTrimmedDecimals)
{
    EXPECT_EQ(formatSeconds(0), "0");
    EXPECT_EQ(formatSeconds(1000), "1");
    EXPECT_EQ(formatSeconds(1500), "1.5");
    EXPECT_EQ(formatSeconds(2250), "2.25");
    EXPECT_EQ(formatSeconds(3125), "3.125");
    EXPECT_EQ(formatSeconds(100), "0.1");
    EXPECT_EQ(formatSeconds(1), "0.001");
    EXPECT_EQ(formatSeconds(3600000), "3600");
}

TEST(DemoOverrideTest, DescribeRoundTripsThroughParse)
{
    const char* script = "0.5:tab=machine, 1:select=pid:1234, 1.25:details=npu, 2:open=about, 2:close=dialog, 3:theme=nord-light, "
                         "4:section=connections, 5:machine=npu";
    EXPECT_EQ(parsed(script), script);
    EXPECT_EQ(describe(std::vector<Step>{}), "");
}

} // namespace
