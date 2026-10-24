/// @file test_SelectOverride.cpp
/// @brief App::SelectOverride (#1559): the TASKSMACK_SELECT_PID / _NAME / TASKSMACK_DETAILS_TAB parser,
/// the pending selection that fires exactly once, when the process first appears in a snapshot, and the
/// TASKSMACK_TAB and TASKSMACK_MACHINE_TAB (#2869) requests that wait until their tab shows.

#include "App/SelectOverride.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using App::SelectOverride::DetailsTab;
using App::SelectOverride::parse;
using App::SelectOverride::Pending;
using App::SelectOverride::Target;

Domain::ProcessSnapshot proc(const std::int32_t pid, std::string name)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.pid = pid;
    snapshot.name = std::move(name);
    snapshot.startTimeTicks = static_cast<std::uint64_t>(pid) * 1000U;
    return snapshot;
}

TEST(SelectOverrideTest, UnsetOrBlankSelectsNothingQuietly)
{
    for (const char* value : {static_cast<const char*>(nullptr), "", "   ", "\t\n"})
    {
        const auto result = parse(value, value, "gpu");
        EXPECT_FALSE(result.target.has_value());
        EXPECT_TRUE(result.warnings.empty());
    }
}

std::optional<Target> byPid(const std::int32_t pid, const DetailsTab tab = DetailsTab::Overview)
{
    return Target{.pid = pid, .name = {}, .tab = tab};
}

std::optional<Target> byName(std::string name, const DetailsTab tab = DetailsTab::Overview)
{
    return Target{.pid = std::nullopt, .name = std::move(name), .tab = tab};
}

TEST(SelectOverrideTest, ParsesPidWithSurroundingSpaces)
{
    const auto result = parse(" 1234 ", nullptr, nullptr);
    EXPECT_EQ(result.target, byPid(1234));
    EXPECT_TRUE(result.warnings.empty());
}

TEST(SelectOverrideTest, RejectsJunkNegativeZeroAndOverflowingPids)
{
    for (const char* value : {"abc", "12x", "-5", "+5", "0", "1 2", "2147483648", "99999999999999999999"})
    {
        const auto result = parse(value, nullptr, nullptr);
        EXPECT_FALSE(result.target.has_value()) << value;
        EXPECT_EQ(result.warnings.size(), 1U) << value;
    }
    EXPECT_EQ(App::SelectOverride::parsePid("2147483647"), std::optional<std::int32_t>{2147483647});
}

TEST(SelectOverrideTest, InvalidPidFallsBackToName)
{
    const auto result = parse("junk", " explorer.exe ", nullptr);
    EXPECT_EQ(result.target, byName("explorer.exe"));
    EXPECT_EQ(result.warnings.size(), 1U);
}

TEST(SelectOverrideTest, ParsesTabsInAnyCase)
{
    EXPECT_EQ(parse("1", nullptr, "overview").target, byPid(1, DetailsTab::Overview));
    EXPECT_EQ(parse("1", nullptr, " GPU ").target, byPid(1, DetailsTab::Gpu));
    EXPECT_EQ(parse("1", nullptr, "npu").target, byPid(1, DetailsTab::Npu)); // #1869
    EXPECT_EQ(parse("1", nullptr, "Network").target, byPid(1, DetailsTab::Network));
    EXPECT_EQ(parse("1", nullptr, "").target, byPid(1, DetailsTab::Overview));
}

TEST(SelectOverrideTest, UnknownTabWarnsAndUsesOverview)
{
    const auto result = parse(nullptr, "explorer.exe", "actions");
    EXPECT_EQ(result.target, byName("explorer.exe", DetailsTab::Overview));
    EXPECT_EQ(result.warnings.size(), 1U);
}

TEST(SelectOverrideTest, PidWinsOverName)
{
    const Target target{.pid = 7, .name = "explorer.exe", .tab = DetailsTab::Overview};
    EXPECT_TRUE(App::SelectOverride::matches(target, proc(7, "other.exe")));
    EXPECT_FALSE(App::SelectOverride::matches(target, proc(8, "explorer.exe")));
}

TEST(SelectOverrideTest, NameMatchFollowsPlatformCase)
{
    const Target target{.pid = std::nullopt, .name = "Explorer.EXE", .tab = DetailsTab::Overview};
    EXPECT_TRUE(App::SelectOverride::matches(target, proc(1, "Explorer.EXE")));
#ifdef _WIN32
    EXPECT_TRUE(App::SelectOverride::matches(target, proc(1, "explorer.exe")));
#else
    EXPECT_FALSE(App::SelectOverride::matches(target, proc(1, "explorer.exe")));
#endif
}

TEST(SelectOverrideTest, NonAsciiNameCaseFollowsWindowsFileNames)
{
    // "\xC3\x89" is É and "\xC3\xA9" is é: the same file name on Windows, different ones elsewhere.
    const Target target{.pid = std::nullopt, .name = "\xC3\x89.exe", .tab = DetailsTab::Overview};
    EXPECT_TRUE(App::SelectOverride::matches(target, proc(1, "\xC3\x89.exe")));
#ifdef _WIN32
    EXPECT_TRUE(App::SelectOverride::matches(target, proc(1, "\xC3\xA9.exe")));
    EXPECT_TRUE(App::SelectOverride::tabNamesEqual("\xC3\x89"
                                                   "clair",
                                                   "\xC3\xA9"
                                                   "CLAIR"));
#else
    EXPECT_FALSE(App::SelectOverride::matches(target, proc(1, "\xC3\xA9.exe")));
    EXPECT_FALSE(App::SelectOverride::tabNamesEqual("\xC3\x89"
                                                    "clair",
                                                    "\xC3\xA9"
                                                    "CLAIR")); // ASCII-only folding
    EXPECT_TRUE(App::SelectOverride::tabNamesEqual("\xC3\x89"
                                                   "clair",
                                                   "\xC3\x89"
                                                   "CLAIR"));
#endif
}

TEST(SelectOverrideTest, NamesThatAreNotUtf8CompareExactly)
{
    // "\xFF" is never valid UTF-8, so Windows can't fold its case through UTF-16 and falls back to an
    // exact comparison (#1566): the same bytes match, a case difference elsewhere in the name doesn't.
    // Other platforms compare process names exactly anyway.
    EXPECT_TRUE(App::SelectOverride::processNamesEqual("\xFF"
                                                       "app.exe",
                                                       "\xFF"
                                                       "app.exe"));
    EXPECT_FALSE(App::SelectOverride::processNamesEqual("\xFF"
                                                        "app.exe",
                                                        "\xFF"
                                                        "APP.exe"));
    EXPECT_FALSE(App::SelectOverride::processNamesEqual("\xFF"
                                                        "app.exe",
                                                        "app.exe"));
#ifdef _WIN32
    EXPECT_FALSE(App::SelectOverride::tabNamesEqual("\xFF"
                                                    "gpu",
                                                    "\xFF"
                                                    "GPU"));
#endif
    EXPECT_TRUE(App::SelectOverride::tabNamesEqual("\xFF"
                                                   "gpu",
                                                   "\xFF"
                                                   "gpu"));
}

TEST(SelectOverrideTest, FiredSelectionHandsOverItsDetailsTabOnce)
{
    Pending pending(Target{.pid = 42, .name = {}, .tab = DetailsTab::Gpu});
    EXPECT_FALSE(pending.takeFiredTab().has_value()); // nothing before it fires
    const std::vector<Domain::ProcessSnapshot> with{proc(42, "target")};
    ASSERT_TRUE(pending.onSnapshot(1, with).match.has_value());
    EXPECT_EQ(pending.takeFiredTab(), std::optional<DetailsTab>{DetailsTab::Gpu});
    EXPECT_FALSE(pending.takeFiredTab().has_value()); // taken once
}

TEST(SelectOverrideTest, GivingUpLeavesNoDetailsTabRequest)
{
    // A PID that never appears, with TASKSMACK_DETAILS_TAB=gpu: after the timeout nothing is pending.
    Pending pending(Target{.pid = 42, .name = {}, .tab = DetailsTab::Gpu});
    const std::vector<Domain::ProcessSnapshot> without{proc(1, "a")};
    bool gaveUp = false;
    for (std::uint64_t generation = 0; !gaveUp && generation < 100; ++generation)
    {
        gaveUp = pending.onSnapshot(generation, without).gaveUp;
    }
    ASSERT_TRUE(gaveUp);
    EXPECT_FALSE(pending.pending());
    EXPECT_FALSE(pending.takeFiredTab().has_value());
}

TEST(SelectOverrideTest, FiresExactlyOnceWhenThePidFirstAppears)
{
    Pending pending(Target{.pid = 42, .name = {}, .tab = DetailsTab::Overview});
    const std::vector<Domain::ProcessSnapshot> without{proc(1, "a"), proc(2, "b")};
    const std::vector<Domain::ProcessSnapshot> with{proc(1, "a"), proc(42, "target"), proc(2, "b")};

    EXPECT_FALSE(pending.onSnapshot(1, without).match.has_value());
    EXPECT_FALSE(pending.onSnapshot(2, without).match.has_value());
    EXPECT_TRUE(pending.pending());

    const Pending::Step step = pending.onSnapshot(3, with);
    EXPECT_EQ(step.match, std::optional<std::size_t>{1});
    EXPECT_FALSE(step.gaveUp);
    EXPECT_FALSE(pending.pending());

    // Later generations, with the process still there, never select it again.
    for (std::uint64_t generation = 4; generation < 40; ++generation)
    {
        const Pending::Step later = pending.onSnapshot(generation, with);
        EXPECT_FALSE(later.match.has_value());
        EXPECT_FALSE(later.gaveUp);
    }
}

TEST(SelectOverrideTest, FirstNameMatchIsSelected)
{
    Pending pending(Target{.pid = std::nullopt, .name = "svchost.exe", .tab = DetailsTab::Overview});
    const std::vector<Domain::ProcessSnapshot> snapshots{proc(1, "a"), proc(5, "svchost.exe"), proc(9, "svchost.exe")};
    EXPECT_EQ(pending.onSnapshot(0, snapshots).match, std::optional<std::size_t>{1});
}

TEST(SelectOverrideTest, SameGenerationCountsOnce)
{
    Pending pending(Target{.pid = 42, .name = {}, .tab = DetailsTab::Overview});
    const std::vector<Domain::ProcessSnapshot> without{proc(1, "a")};
    // Frames that see no new generation do not use up the snapshot budget.
    for (int frame = 0; frame < 3 * App::SelectOverride::MAX_SNAPSHOTS; ++frame)
    {
        EXPECT_FALSE(pending.onSnapshot(7, without).gaveUp);
    }
    EXPECT_TRUE(pending.pending());
}

TEST(SelectOverrideTest, NeverFiresAndGivesUpOnceIfThePidNeverAppears)
{
    Pending pending(Target{.pid = 42, .name = {}, .tab = DetailsTab::Overview});
    const std::vector<Domain::ProcessSnapshot> without{proc(1, "a"), proc(2, "b")};
    int gaveUp = 0;
    for (std::uint64_t generation = 0; generation < 100; ++generation)
    {
        const Pending::Step step = pending.onSnapshot(generation, without);
        EXPECT_FALSE(step.match.has_value());
        gaveUp += step.gaveUp ? 1 : 0;
        EXPECT_EQ(pending.pending(), generation + 1 < static_cast<std::uint64_t>(App::SelectOverride::MAX_SNAPSHOTS));
    }
    EXPECT_EQ(gaveUp, 1);

    // Once given up, the process appearing later is not selected either.
    const std::vector<Domain::ProcessSnapshot> with{proc(42, "late")};
    EXPECT_FALSE(pending.onSnapshot(200, with).match.has_value());
}

TEST(SelectOverrideTest, NoTargetIsNeverPending)
{
    Pending pending;
    EXPECT_FALSE(pending.pending());
    const std::vector<Domain::ProcessSnapshot> snapshots{proc(1, "a")};
    const Pending::Step step = pending.onSnapshot(0, snapshots);
    EXPECT_FALSE(step.match.has_value());
    EXPECT_FALSE(step.gaveUp);
}

// As ShellLayer registers them, plus one registered later, matched only by its own id or text.
const std::vector<App::SelectOverride::TabInfo>& registeredTabs()
{
    static const std::vector<App::SelectOverride::TabInfo> tabs{
        {.id = "SystemOverview", .text = "MYHOST"},
        {.id = "Processes", .text = "Processes"},
        {.id = "ProcessDetails", .text = "explorer.exe"},
        {.id = "Services", .text = "Services"},
        {.id = "SystemInfo", .text = "System"},
    };
    return tabs;
}

TEST(SelectOverrideTest, FindsTopLevelTabsByIdOrTextIgnoringCase)
{
    using App::SelectOverride::findTab;
    const auto& tabs = registeredTabs();
    EXPECT_EQ(findTab("system", tabs), std::optional<std::size_t>{4}); // the System Information tab's text (#1399)
    EXPECT_EQ(findTab("Machine", tabs), std::optional<std::size_t>{0});
    EXPECT_EQ(findTab("overview", tabs), std::optional<std::size_t>{0});
    EXPECT_EQ(findTab("myhost", tabs), std::optional<std::size_t>{0});
    EXPECT_EQ(findTab(" PROCESSES ", tabs), std::optional<std::size_t>{1});
    EXPECT_EQ(findTab("details", tabs), std::optional<std::size_t>{2});
    EXPECT_EQ(findTab("processdetails", tabs), std::optional<std::size_t>{2});
    EXPECT_EQ(findTab("explorer.exe", tabs), std::optional<std::size_t>{2});
    EXPECT_EQ(findTab("services", tabs), std::optional<std::size_t>{3});
}

TEST(SelectOverrideTest, ExactIdsWinAndThereIsNoPrefixOrSuffixMatching)
{
    using App::SelectOverride::findTab;
    using App::SelectOverride::TabInfo;
    const std::vector<TabInfo> tabs{
        {.id = "SystemOverview", .text = "MYHOST"},
        {.id = "Overview", .text = "Summary"},
        {.id = "ProcessDetails", .text = "explorer.exe"},
    };
    EXPECT_EQ(findTab("overview", tabs), std::optional<std::size_t>{1}); // not SystemOverview's suffix
    EXPECT_EQ(findTab("machine", tabs), std::optional<std::size_t>{0});  // a documented alias
    EXPECT_FALSE(findTab("process", tabs).has_value());
    EXPECT_FALSE(findTab("systemover", tabs).has_value());
    EXPECT_FALSE(findTab("view", tabs).has_value());
}

TEST(SelectOverrideTest, MatchesNonAsciiAndHashTextAsIs)
{
    // The text is the unescaped display text, so a hostname starting with a non-ASCII letter and a
    // name holding "##" match whole (the escaped label would hide or mangle both).
    using App::SelectOverride::findTab;
    using App::SelectOverride::TabInfo;
    const std::vector<TabInfo> tabs{
        {.id = "SystemOverview",
         .text = "\xC3\x89"
                 "clair"}, // "Éclair"
        {.id = "ProcessDetails", .text = "a##b"},
    };
    EXPECT_EQ(findTab("\xC3\x89"
                      "clair",
                      tabs),
              std::optional<std::size_t>{0});
    EXPECT_FALSE(findTab("clair", tabs).has_value());
    EXPECT_EQ(findTab("a##b", tabs), std::optional<std::size_t>{1});
    EXPECT_FALSE(findTab("a#b", tabs).has_value());
}

TEST(SelectOverrideTest, UnknownOrBlankTopLevelTabFindsNothing)
{
    using App::SelectOverride::findTab;
    for (const char* name : {"", "   ", "startup", "processesx", "###ProcessesTab"})
    {
        EXPECT_FALSE(findTab(name, registeredTabs()).has_value()) << name;
    }
}

TEST(SelectOverrideTest, UnknownMainTabWarnsOnceAndStillOpensDetails)
{
    using App::SelectOverride::resolveMainTab;
    using App::SelectOverride::selectionShowsDetails;

    const auto unknown = resolveMainTab("processesx", registeredTabs());
    EXPECT_FALSE(unknown.id.has_value());
    EXPECT_FALSE(unknown.warning.empty());
    EXPECT_TRUE(selectionShowsDetails(unknown)); // ignored: the selection still opens Process Details

    const auto known = resolveMainTab("processes", registeredTabs());
    EXPECT_EQ(known.id, std::optional<std::string>{"Processes"});
    EXPECT_TRUE(known.warning.empty());
    EXPECT_FALSE(selectionShowsDetails(known)); // TASKSMACK_TAB wins

    for (const char* blank : {static_cast<const char*>(nullptr), "", "  "})
    {
        const auto none = resolveMainTab(blank, registeredTabs());
        EXPECT_FALSE(none.id.has_value());
        EXPECT_TRUE(none.warning.empty());
        EXPECT_TRUE(selectionShowsDetails(none));
    }
}

// #1569: the startup "Limited Data" notice is not queued while any hook is set, TASKSMACK_TAB alone included.
TEST(SelectOverrideTest, MainTabAloneIsATestHook)
{
    using App::SelectOverride::anyTestHookActive;

    EXPECT_TRUE(anyTestHookActive("processes", nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_TRUE(anyTestHookActive("not-a-tab", nullptr, nullptr, nullptr, nullptr, nullptr)); // set, even if ignored
    EXPECT_TRUE(anyTestHookActive(nullptr, "1234", nullptr, nullptr, nullptr, nullptr));
    EXPECT_TRUE(anyTestHookActive(nullptr, nullptr, "explorer.exe", nullptr, nullptr, nullptr));
    EXPECT_TRUE(anyTestHookActive(nullptr, nullptr, nullptr, "gpu", nullptr, nullptr));
    EXPECT_TRUE(anyTestHookActive(nullptr, nullptr, nullptr, nullptr, "help", nullptr));  // #172
    EXPECT_TRUE(anyTestHookActive(nullptr, nullptr, nullptr, nullptr, nullptr, "cores")); // #2869
}

TEST(SelectOverrideTest, NoHooksMeansNoTestHook)
{
    using App::SelectOverride::anyTestHookActive;

    EXPECT_FALSE(anyTestHookActive(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_FALSE(anyTestHookActive("", "  ", "\t", "", " ", "\r\n")); // blank counts as unset
}

// #172: TASKSMACK_OPEN opens the Help window or the About dialog at startup.
TEST(SelectOverrideTest, ParsesTheStartupDialog)
{
    using App::SelectOverride::parseStartupDialog;
    using App::SelectOverride::StartupDialog;

    EXPECT_EQ(parseStartupDialog("help").dialog, StartupDialog::Help);
    EXPECT_EQ(parseStartupDialog(" HELP ").dialog, StartupDialog::Help);
    EXPECT_EQ(parseStartupDialog("About").dialog, StartupDialog::About);
    EXPECT_TRUE(parseStartupDialog("help").warning.empty());

    const auto unknown = parseStartupDialog("settings");
    EXPECT_FALSE(unknown.dialog.has_value());
    EXPECT_FALSE(unknown.warning.empty());

    for (const char* blank : {static_cast<const char*>(nullptr), "", "  "})
    {
        const auto none = parseStartupDialog(blank);
        EXPECT_FALSE(none.dialog.has_value());
        EXPECT_TRUE(none.warning.empty());
    }
}

// #2827: the names TASKSMACK_DEMO's steps use for the tabs, the Overview's sections, the dialogs and
// (#2869) the machine tab's sub-tabs, in any case, and written back the same way for a log line.
TEST(SelectOverrideTest, NamesOfTabsSectionsAndDialogsParseInAnyCaseAndWriteBack)
{
    using App::SelectOverride::DetailsSection;
    using App::SelectOverride::dialogName;
    using App::SelectOverride::MachineTab;
    using App::SelectOverride::machineTabName;
    using App::SelectOverride::parseDialog;
    using App::SelectOverride::parseMachineTab;
    using App::SelectOverride::parseSection;
    using App::SelectOverride::parseTab;
    using App::SelectOverride::sectionName;
    using App::SelectOverride::StartupDialog;
    using App::SelectOverride::tabName;

    for (const auto& [name, tab] : App::SelectOverride::TAB_NAMES)
    {
        EXPECT_EQ(parseTab(name), tab);
        EXPECT_EQ(tabName(tab), name);
    }
    for (const auto& [name, tab] : App::SelectOverride::MACHINE_TAB_NAMES)
    {
        EXPECT_EQ(parseMachineTab(name), tab);
        EXPECT_EQ(machineTabName(tab), name);
    }
    EXPECT_EQ(parseMachineTab("Cores"), MachineTab::Cores);
    EXPECT_EQ(parseMachineTab("GPU"), MachineTab::Gpu);
    EXPECT_EQ(parseMachineTab("nPu"), MachineTab::Npu);
    EXPECT_EQ(parseMachineTab("NETWORK"), MachineTab::Network);
    EXPECT_EQ(parseMachineTab("OverView"), MachineTab::Overview);
    EXPECT_FALSE(parseMachineTab("memory").has_value());
    EXPECT_FALSE(parseMachineTab("cpu cores").has_value());
    EXPECT_FALSE(parseMachineTab("core").has_value()); // no prefix matching
    EXPECT_FALSE(parseMachineTab("").has_value());
    for (const auto& [name, section] : App::SelectOverride::SECTION_NAMES)
    {
        EXPECT_EQ(parseSection(name), section);
        EXPECT_EQ(sectionName(section), name);
    }
    for (const auto& [name, dialog] : App::SelectOverride::DIALOG_NAMES)
    {
        EXPECT_EQ(parseDialog(name), dialog);
        EXPECT_EQ(dialogName(dialog), name);
    }
    EXPECT_EQ(parseSection("OpenFiles"), DetailsSection::OpenFiles);
    EXPECT_EQ(parseSection("MODULES"), DetailsSection::Modules);
    EXPECT_EQ(parseDialog("About"), StartupDialog::About);
    EXPECT_FALSE(parseSection("files").has_value());
    EXPECT_FALSE(parseSection("open files").has_value());
    EXPECT_FALSE(parseSection("").has_value());
    EXPECT_FALSE(parseDialog("settings").has_value());
    EXPECT_FALSE(parseTab("memory").has_value());
}

// #1575: TASKSMACK_TAB resolves to the tab's registered id, never to its position in the list.
TEST(SelectOverrideTest, MainTabResolvesToTheRegisteredIdNotAnIndex)
{
    using App::SelectOverride::resolveMainTab;
    using App::SelectOverride::TabInfo;
    const std::vector<TabInfo> tabs{
        {.id = "SystemOverview", .text = "MYHOST"},
        {.id = "Services", .text = "Services"},
        {.id = "Startup", .text = "Startup"},
    };
    EXPECT_EQ(resolveMainTab("startup", tabs).id, std::optional<std::string>{"Startup"});
    EXPECT_EQ(resolveMainTab("SERVICES", tabs).id, std::optional<std::string>{"Services"});
    EXPECT_EQ(resolveMainTab("myhost", tabs).id, std::optional<std::string>{"SystemOverview"});
    EXPECT_EQ(resolveMainTab("machine", tabs).id, std::optional<std::string>{"SystemOverview"});
}

// #1575: the request is asked for on every frame and stays until its own tab reports selected.
TEST(SelectOverrideTest, PendingMainTabStaysUntilItsTabReportsSelected)
{
    using App::SelectOverride::PendingMainTab;
    PendingMainTab request(std::optional<std::string>{"Startup"});
    ASSERT_TRUE(request.pending());
    EXPECT_TRUE(request.wantsSelected("Startup"));
    EXPECT_FALSE(request.wantsSelected("Services"));
    EXPECT_FALSE(request.wantsSelected("startup")); // the registered id, exactly

    // The first frame: ImGui shows another tab (the first one, as it does on a tab bar's first frame).
    request.onTabSubmitted("SystemOverview", true);
    request.onTabSubmitted("Services", false);
    request.onTabSubmitted("Startup", false);
    EXPECT_FALSE(request.onFrameEnd().has_value());
    EXPECT_TRUE(request.pending());
    EXPECT_TRUE(request.wantsSelected("Startup"));

    // A neighbour reporting selected does not count.
    request.onTabSubmitted("Services", true);
    EXPECT_TRUE(request.pending());

    // Its own tab does: the request is done and asks for nothing more.
    request.onTabSubmitted("Startup", true);
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.wantsSelected("Startup"));
    EXPECT_FALSE(request.onFrameEnd().has_value());
}

TEST(SelectOverrideTest, PendingMainTabIsDroppedOnceAfterTheFrameLimit)
{
    using App::SelectOverride::MAX_MAIN_TAB_FRAMES;
    using App::SelectOverride::PendingMainTab;
    PendingMainTab request(std::optional<std::string>{"Startup"});
    for (int frame = 1; frame < MAX_MAIN_TAB_FRAMES; ++frame)
    {
        ASSERT_FALSE(request.onFrameEnd().has_value()) << frame;
    }
    EXPECT_EQ(request.onFrameEnd(), std::optional<std::string>{"Startup"});
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.onFrameEnd().has_value()); // reported once

    PendingMainTab none;
    EXPECT_FALSE(none.pending());
    EXPECT_FALSE(none.wantsSelected(""));
    EXPECT_FALSE(none.onFrameEnd().has_value());
}

// #2869: TASKSMACK_MACHINE_TAB names a sub-tab of the machine tab, in any case; anything else is
// ignored with one warning naming the variable, and unset or blank is ignored quietly.
TEST(SelectOverrideTest, ParsesTheStartupMachineTab)
{
    using App::SelectOverride::MachineTab;
    using App::SelectOverride::parseStartupMachineTab;

    EXPECT_EQ(parseStartupMachineTab("gpu").tab, MachineTab::Gpu);
    EXPECT_EQ(parseStartupMachineTab(" CORES\r\n").tab, MachineTab::Cores);
    EXPECT_EQ(parseStartupMachineTab("Network").tab, MachineTab::Network);
    EXPECT_EQ(parseStartupMachineTab("npu").tab, MachineTab::Npu);
    EXPECT_EQ(parseStartupMachineTab("overview").tab, MachineTab::Overview);
    EXPECT_TRUE(parseStartupMachineTab("gpu").warning.empty());

    const auto unknown = parseStartupMachineTab("memory");
    EXPECT_FALSE(unknown.tab.has_value());
    EXPECT_NE(unknown.warning.find("TASKSMACK_MACHINE_TAB"), std::string::npos) << unknown.warning;
    EXPECT_NE(unknown.warning.find("'memory'"), std::string::npos) << unknown.warning;
    EXPECT_NE(unknown.warning.find("ignored"), std::string::npos) << unknown.warning;

    for (const char* blank : {static_cast<const char*>(nullptr), "", "  ", "\t\r\n"})
    {
        const auto none = parseStartupMachineTab(blank);
        EXPECT_FALSE(none.tab.has_value());
        EXPECT_TRUE(none.warning.empty());
    }
}

// #2869: as PendingMainTab, the request is asked for on every frame its sub-tab is drawn and stays until
// that sub-tab reports selected; another sub-tab's report, or a frame without the sub-tab, changes nothing.
TEST(SelectOverrideTest, PendingMachineTabStaysUntilItsTabReportsSelected)
{
    using App::SelectOverride::MachineTab;
    using App::SelectOverride::PendingMachineTab;
    PendingMachineTab request(MachineTab::Network);
    ASSERT_TRUE(request.pending());
    EXPECT_EQ(request.tab(), MachineTab::Network);
    EXPECT_TRUE(request.wantsSelected(MachineTab::Network));
    EXPECT_FALSE(request.wantsSelected(MachineTab::Overview));

    // The first frame: the Overview is selected, the Network tab is submitted with the flag.
    request.onTabSubmitted(MachineTab::Overview, true);
    request.onTabSubmitted(MachineTab::Gpu, false);
    request.onTabSubmitted(MachineTab::Network, false);
    EXPECT_FALSE(request.onFrameEnd().has_value());
    EXPECT_TRUE(request.pending());
    EXPECT_TRUE(request.wantsSelected(MachineTab::Network));

    // A neighbour reporting selected does not count.
    request.onTabSubmitted(MachineTab::Gpu, true);
    EXPECT_TRUE(request.pending());

    // Its own does: the request is done and asks for nothing more.
    request.onTabSubmitted(MachineTab::Network, true);
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.tab().has_value());
    EXPECT_FALSE(request.wantsSelected(MachineTab::Network));
    EXPECT_FALSE(request.onFrameEnd().has_value());
}

TEST(SelectOverrideTest, PendingMachineTabIsDroppedOnceAfterTheFrameLimit)
{
    using App::SelectOverride::MachineTab;
    using App::SelectOverride::MAX_MACHINE_TAB_FRAMES;
    using App::SelectOverride::PendingMachineTab;
    PendingMachineTab request(MachineTab::Npu);
    for (int frame = 1; frame < MAX_MACHINE_TAB_FRAMES; ++frame)
    {
        ASSERT_FALSE(request.onFrameEnd().has_value()) << frame;
        ASSERT_TRUE(request.wantsSelected(MachineTab::Npu)) << frame;
    }
    EXPECT_EQ(request.onFrameEnd(), MachineTab::Npu);
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.onFrameEnd().has_value()); // reported once

    PendingMachineTab none;
    EXPECT_FALSE(none.pending());
    EXPECT_FALSE(none.wantsSelected(MachineTab::Overview));
    EXPECT_FALSE(none.onFrameEnd().has_value());
}

// #2170: the same ASCII whitespace as every other TASKSMACK_* hook (Platform::trimAscii()).
TEST(SelectOverrideTest, AnyAsciiWhitespaceAroundThePidIsTrimmed)
{
    const auto result = parse("\v1234\r\n", nullptr, nullptr);
    EXPECT_EQ(result.target, byPid(1234));
    EXPECT_TRUE(result.warnings.empty());
}

} // namespace
