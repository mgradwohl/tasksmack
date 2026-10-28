/// @file test_StartupViewRender.cpp
/// @brief The Startup tab's table (#801), headless: the unsupported message (the Linux stub's
/// capabilities), the loading state, rows for a publication (a missing target among them), the
/// filter, the pure row order and labels, and the Startup impact column and its tooltip (#3073).

#include "App/Panels/StartupActionsView.h"
#include "App/Panels/StartupView.h"
#include "Domain/StartupImpact.h"
#include "Domain/StartupModel.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer, TableFindByID()

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace App
{
namespace
{

using Platform::StartupLocation;

[[nodiscard]] Platform::StartupEntry
entry(const char* name, const char* publisher, StartupLocation location, bool enabled, std::uint64_t disabledAt = 0)
{
    Platform::StartupEntry startup;
    startup.name = name;
    startup.publisher = publisher;
    startup.command = std::string(R"("C:\Apps\)") + name + R"(.exe" --background)";
    startup.executablePath = std::string("C:\\Apps\\") + name + ".exe";
    startup.location = location;
    startup.scope = (location == StartupLocation::RunUser || location == StartupLocation::StartupFolderUser)
                      ? Platform::StartupScope::User
                      : Platform::StartupScope::Machine;
    startup.enabled = enabled;
    startup.disabledAtUnixSeconds = disabledAt;
    startup.target = Platform::StartupTargetState::Present;
    return startup;
}

[[nodiscard]] Domain::StartupPublication makePublication()
{
    Domain::StartupPublication publication;
    publication.version = 4;
    publication.entries = {
        entry("Discord", "Discord Inc.", StartupLocation::RunUser, true),
        entry("OneDrive", "Microsoft Corporation", StartupLocation::RunUser, false, 1704164645),
        entry("SecurityHealth", "Microsoft Corporation", StartupLocation::RunMachine, true),
        entry("Stale", "", StartupLocation::StartupFolderCommon, true),
    };
    publication.entries[3].target = Platform::StartupTargetState::Missing;
    return publication;
}

[[nodiscard]] Platform::StartupCapabilities supported()
{
    return {.canEnumerate = true,
            .hasEnabledState = true,
            .hasDisabledTime = true,
            .hasPublisher = true,
            .canResolveShortcuts = true,
            .hasImpact = true,
            .unavailableReason = {}};
}

/// Can list entries, but without the enabled state or anything optional.
[[nodiscard]] Platform::StartupCapabilities enumerateOnly()
{
    return {.canEnumerate = true,
            .hasEnabledState = false,
            .hasDisabledTime = false,
            .hasPublisher = false,
            .canResolveShortcuts = false,
            .unavailableReason = {}};
}

/// Elevated actions that are never run here: the tests only draw the action bar.
class IdleStartupActions final : public Platform::IStartupActions
{
  public:
    [[nodiscard]] Platform::StartupActionCapabilities capabilities() const override
    {
        return {.canSetEnabled = true, .elevated = true};
    }
    [[nodiscard]] Platform::StartupActionResult setEnabled(const Platform::StartupEntry& /*entry*/, bool /*enabled*/) override
    {
        return Platform::StartupActionResult::succeeded();
    }
};

class StartupViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    static StartupViewContent runFrame(const std::function<StartupViewContent()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Startup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const StartupViewContent content = body();
        ImGui::End();
        ImGui::Render();
        return content;
    }

    /// One frame of the view, returning every text it drew (tooltips included).
    static std::string
    captureFrame(const Domain::StartupPublication& publication, StartupViewState& state, StartupActionsView* actions = nullptr)
    {
        std::string captured;
        static_cast<void>(runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                const StartupViewContent content = renderStartupView(&publication, supported(), state, actions);
                captured = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
                return content;
            }));
        return captured;
    }

    /// The centre of the table's first row's cell in @p column (Name by default), from a frame that drew @p publication.
    static ImVec2 firstRowCentre(const Domain::StartupPublication& publication,
                                 StartupViewState& state,
                                 StartupActionsView* actions = nullptr,
                                 int column = 0)
    {
        ImVec2 centre(-1.0F, -1.0F);
        static_cast<void>(runFrame(
            [&]
            {
                const StartupViewContent content = renderStartupView(&publication, supported(), state, actions);
                if (const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##StartupTable")); table != nullptr)
                {
                    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                    centre = ImVec2((table->Columns[column].MinX + table->Columns[column].MaxX) * 0.5F,
                                    table->OuterRect.Min.y + (rowHeight * 1.5F));
                }
                return content;
            }));
        return centre;
    }

    /// Hovers @p at and draws @p publication until its tooltip shows @p wanted (a few frames, each
    /// past the hover delay); returns the last frame's text.
    static std::string hoverUntil(const Domain::StartupPublication& publication, StartupViewState& state, ImVec2 at, const char* wanted)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        io.DeltaTime = 0.25F;
        std::string text;
        for (int i = 0; i < 6 && !text.contains(wanted); ++i)
        {
            text = captureFrame(publication, state);
        }
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.DeltaTime = 1.0F / 60.0F;
        return text;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(StartupViewRenderTest, UnsupportedPlatformShowsTheMessage)
{
    // The Linux factory's UnsupportedStartupProbe reports these capabilities.
    const Platform::StartupCapabilities unsupported = Platform::UnsupportedStartupProbe{}.capabilities();
    StartupViewState state;
    EXPECT_EQ(runFrame([&] { return renderStartupView(nullptr, unsupported, state); }), StartupViewContent::Unsupported);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    EXPECT_TRUE(state.unavailableHeading.contains("Startup apps aren't available on this platform yet"));
}

TEST_F(StartupViewRenderTest, NoSampleYetShowsLoading)
{
    StartupViewState state;
    const Domain::StartupPublication empty;
    EXPECT_EQ(runFrame([&] { return renderStartupView(&empty, supported(), state); }), StartupViewContent::Loading);
}

TEST_F(StartupViewRenderTest, TableShowsRowsAndFilters)
{
    const auto publication = makePublication();
    StartupViewState state;
    EXPECT_EQ(runFrame([&] { return renderStartupView(&publication, supported(), state); }), StartupViewContent::Table);
    EXPECT_EQ(state.rows.size(), 4U);
    EXPECT_EQ(state.rowsVersion, 4U);
    ASSERT_EQ(state.enabledLabels.size(), 4U);
    EXPECT_EQ(state.enabledLabels[0], "Enabled");
    EXPECT_TRUE(state.enabledLabels[1].starts_with("Disabled since 2024-01-0")) << state.enabledLabels[1];

    // Publisher matches too, case-insensitively; by the default sort the enabled one comes first.
    state.filter = "MICROSOFT";
    static_cast<void>(runFrame([&] { return renderStartupView(&publication, supported(), state); }));
    EXPECT_EQ(state.rows, (std::vector<std::size_t>{2, 1}));

    // Without an enabled state the table still draws.
    state.filter.clear();
    EXPECT_EQ(runFrame([&] { return renderStartupView(&publication, enumerateOnly(), state); }), StartupViewContent::Table);
}

TEST(StartupViewTest, RowsSortByColumnAndDirection)
{
    const auto publication = makePublication();
    // Enabled first (in name order), then the disabled one.
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Enabled, true), (std::vector<std::size_t>{0, 2, 3, 1}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Name, false), (std::vector<std::size_t>{3, 2, 1, 0}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Location, true), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Scope, true), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Publisher, true), (std::vector<std::size_t>{3, 0, 1, 2}));
    EXPECT_EQ(buildStartupRows(publication.entries, "background", StartupColumn::Name, true).size(), 4U);
    EXPECT_TRUE(buildStartupRows(publication.entries, "nothing matches", StartupColumn::Name, true).empty());
}

TEST_F(StartupViewRenderTest, DefaultSortIsEnabledFirstThenName)
{
    // #1597: with no header clicked, the table is sorted by Enabled -- enabled entries A to Z, then the
    // disabled ones A to Z -- and the table's own default sort spec agrees, so the first frame keeps it.
    const auto publication = makePublication();
    StartupViewState state;
    EXPECT_EQ(state.sortColumn, StartupColumn::Enabled);
    EXPECT_TRUE(state.ascending);
    static_cast<void>(runFrame([&] { return renderStartupView(&publication, supported(), state); }));
    static_cast<void>(runFrame([&] { return renderStartupView(&publication, supported(), state); }));
    EXPECT_EQ(state.sortColumn, StartupColumn::Enabled);
    EXPECT_TRUE(state.ascending);
    EXPECT_EQ(state.rows, (std::vector<std::size_t>{0, 2, 3, 1}));
}

TEST(StartupViewTest, EnabledSortGroupsThenNamesCaseInsensitively)
{
    const std::vector<Platform::StartupEntry> entries = {
        entry("zoom", "", StartupLocation::RunUser, true),
        entry("Backup", "", StartupLocation::RunUser, false, 1704164645), // disabled earliest
        entry("adobe", "", StartupLocation::RunUser, false, 1804164645),
        entry("Teams", "", StartupLocation::RunUser, true),
        entry("ARC", "", StartupLocation::RunUser, true),
        entry("Cortex", "", StartupLocation::RunUser, false),
    };
    // Enabled A to Z (case ignored), then disabled A to Z -- not by when they were disabled.
    EXPECT_EQ(buildStartupRows(entries, "", StartupColumn::Enabled, true), (std::vector<std::size_t>{4, 3, 0, 2, 1, 5}));
    // Descending reverses the groups only: the names stay A to Z within each.
    EXPECT_EQ(buildStartupRows(entries, "", StartupColumn::Enabled, false), (std::vector<std::size_t>{2, 1, 5, 4, 3, 0}));
}

TEST(StartupViewTest, EnabledSortBreaksNameTiesDeterministically)
{
    // Same name differing only in case, and the same name registered in two places.
    const std::vector<Platform::StartupEntry> entries = {
        entry("Updater", "", StartupLocation::StartupFolderUser, true),
        entry("updater", "", StartupLocation::RunUser, true),
        entry("Updater", "", StartupLocation::RunUser, true),
        entry("Updater", "", StartupLocation::RunMachine, false),
    };
    // Exact name ("U" before "u"), then the location; the disabled one last.
    const std::vector<std::size_t> expected = {2, 0, 1, 3};
    EXPECT_EQ(buildStartupRows(entries, "", StartupColumn::Enabled, true), expected);
    // The same order whatever order the entries arrive in.
    const std::vector<Platform::StartupEntry> reversed(entries.rbegin(), entries.rend());
    EXPECT_EQ(buildStartupRows(reversed, "", StartupColumn::Enabled, true), (std::vector<std::size_t>{1, 3, 2, 0}));
}

TEST(StartupViewTest, Labels)
{
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunMachine32), "Registry: HKLM Run (32-bit)");
    EXPECT_EQ(startupLocationLabel(StartupLocation::StartupFolderUser), "Startup folder");
    EXPECT_EQ(startupScopeLabel(Platform::StartupScope::Machine), "All users");
    EXPECT_EQ(startupScopeLabel(Platform::StartupScope::User), "Current user");

    Platform::StartupEntry disabled;
    disabled.enabled = false;
    EXPECT_EQ(startupEnabledLabel(disabled), "Disabled"); // no time recorded
    disabled.enabled = true;
    EXPECT_EQ(startupEnabledLabel(disabled), "Enabled");
    // #2541: Linux's autostart entries, and one another desktop starts.
    EXPECT_EQ(startupLocationLabel(StartupLocation::XdgAutostartUser), "Autostart (your folder)");
    EXPECT_EQ(startupLocationLabel(StartupLocation::XdgAutostartSystem), "Autostart (system)");
    Platform::StartupEntry otherDesktop;
    otherDesktop.enabled = false;
    otherDesktop.notSwitchableReason = "Starts only in KDE";
    EXPECT_EQ(startupEnabledLabel(otherDesktop), "Not started here");
}

// The Windows-only Startup tab's remaining branches (#1395, Windows half).
TEST(StartupViewTest, EveryLocationHasItsLabelAndCommandSorts)
{
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunUser), "Registry: HKCU Run");
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunMachine), "Registry: HKLM Run");
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunOnceUser), "Registry: HKCU RunOnce");
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunOnceMachine), "Registry: HKLM RunOnce");
    EXPECT_EQ(startupLocationLabel(StartupLocation::StartupFolderCommon), "Startup folder (all users)");
    EXPECT_EQ(startupLocationLabel(StartupLocation::PackagedApp), "Packaged app");

    auto publication = makePublication();
    publication.entries[0].command = "zz.exe";
    publication.entries[1].command = "AA.exe";
    publication.entries[2].command = "mm.exe";
    publication.entries[3].command = "bb.exe";
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Command, true), (std::vector<std::size_t>{1, 3, 2, 0}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Command, false), (std::vector<std::size_t>{0, 2, 3, 1}));
}

TEST_F(StartupViewRenderTest, AMissingProgramsTooltipShowsWithoutTheHoverDelay)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    publication.entries = {entry("Stale", "", StartupLocation::RunUser, true)};
    publication.entries[0].target = Platform::StartupTargetState::Missing;
    StartupViewState state;
    const ImVec2 row = firstRowCentre(publication, state);
    ASSERT_GT(row.y, 0.0F);

    const std::string text = hoverUntil(publication, state, row, "was not found");
    EXPECT_TRUE(text.contains("The program this entry starts was not found"));
    EXPECT_TRUE(text.contains(R"(Command: "C:\Apps\Stale.exe" --background)"));
    EXPECT_TRUE(text.contains(R"(Program: C:\Apps\Stale.exe)"));
}

// #1920: a name or publisher too wide for its column ends in an ellipsis rather than being cut off, and
// the whole of it is on hover -- for the Name cell in the entry's tooltip, for any other clipped cell
// in its own tooltip, which stands in for the entry's.
TEST_F(StartupViewRenderTest, ClippedCellsShowTheirWholeTextOnHover)
{
    const std::string LONG_NAME = "Windows Security notification icon with an even longer name";
    const std::string LONG_PUBLISHER = "Intel(R) Management and Security Application Publisher Corporation";
    Domain::StartupPublication publication;
    publication.version = 1;
    publication.entries = {entry(LONG_NAME.c_str(), LONG_PUBLISHER.c_str(), StartupLocation::RunUser, true)};
    StartupViewState state;

    const ImVec2 name = firstRowCentre(publication, state);
    ASSERT_GT(name.y, 0.0F);
    const std::string nameText = hoverUntil(publication, state, name, "Program: ");
    EXPECT_TRUE(nameText.contains("Program: C:\\Apps\\" + LONG_NAME + ".exe")) << "the entry's tooltip";

    // Column 1, Publisher: hovered for every frame (nothing draws the text waited for).
    const ImVec2 publisher = firstRowCentre(publication, state, nullptr, 1);
    const std::string publisherText = hoverUntil(publication, state, publisher, "not drawn: hover every frame");
    const std::size_t first = publisherText.find(LONG_PUBLISHER);
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(publisherText.find(LONG_PUBLISHER, first + LONG_PUBLISHER.size()), std::string::npos)
        << "drawn in the cell, then in its tooltip";
    EXPECT_FALSE(publisherText.contains("Program: ")) << "the entry's tooltip gives way to the cell's";
}

TEST_F(StartupViewRenderTest, AnUnresolvedShortcutShowsItsOwnPath)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    publication.entries = {entry("Helper", "", StartupLocation::StartupFolderUser, true)};
    auto& shortcut = publication.entries[0];
    shortcut.command.clear();
    shortcut.executablePath.clear();
    shortcut.sourcePath = R"(C:\Users\me\Startup\Helper.lnk)";
    shortcut.target = Platform::StartupTargetState::Unresolved;
    StartupViewState state;

    // The Command cell names the shortcut instead of a command.
    EXPECT_TRUE(captureFrame(publication, state).contains(R"(C:\Users\me\Startup\Helper.lnk (target unresolved))"));

    const ImVec2 row = firstRowCentre(publication, state);
    ASSERT_GT(row.y, 0.0F);
    const std::string text = hoverUntil(publication, state, row, "Shortcut target unresolved");
    EXPECT_TRUE(text.contains("Shortcut target unresolved"));
    EXPECT_TRUE(text.contains(R"(Shortcut: C:\Users\me\Startup\Helper.lnk)"));
    EXPECT_FALSE(text.contains("Command:"));
}

TEST_F(StartupViewRenderTest, RightClickSelectsTheRowTheBarActsOn)
{
    const auto publication = makePublication();
    StartupActionsView actions(std::make_shared<IdleStartupActions>());
    StartupViewState state;
    EXPECT_TRUE(captureFrame(publication, state, &actions).contains("Select a startup app"));

    // The default order lists Discord first; a right click on it selects it, location included.
    const ImVec2 row = firstRowCentre(publication, state, &actions);
    ASSERT_GT(row.y, 0.0F);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(row.x, row.y);
    static_cast<void>(captureFrame(publication, state, &actions));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    static_cast<void>(captureFrame(publication, state, &actions));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    static_cast<void>(captureFrame(publication, state, &actions));
    EXPECT_EQ(state.selectedName, "Discord");
    EXPECT_EQ(state.selectedLocation, StartupLocation::RunUser);
    EXPECT_FALSE(captureFrame(publication, state, &actions).contains("Select a startup app"));

    // The same name registered somewhere else is not the selection.
    state.selectedLocation = StartupLocation::RunMachine;
    EXPECT_TRUE(captureFrame(publication, state, &actions).contains("Select a startup app"));
}

// #1873: a Run entry is shown by its program's name and registered under its value name.

/// An Edge-style Run entry: shown as "Microsoft Edge", registered as an auto-launch value.
[[nodiscard]] Platform::StartupEntry edgeEntry()
{
    auto edge = entry("Microsoft Edge", "Microsoft Corporation", StartupLocation::RunUser, true);
    edge.registrationName = "MicrosoftEdgeAutoLaunch_575A7C";
    return edge;
}

TEST(StartupViewTest, RegistrationLabelNamesTheValueOrTheTask)
{
    EXPECT_EQ(startupRegistrationLabel(edgeEntry()), "Registry value: MicrosoftEdgeAutoLaunch_575A7C");
    EXPECT_EQ(startupEntryKey(edgeEntry()), "MicrosoftEdgeAutoLaunch_575A7C");

    auto same = entry("OneDrive", "", StartupLocation::RunOnceUser, true);
    same.registrationName = "OneDrive";
    EXPECT_EQ(startupRegistrationLabel(same), "") << "nothing to add when the names agree";
    EXPECT_EQ(startupEntryKey(entry("Legacy", "", StartupLocation::RunUser, true)), "Legacy") << "no registered name: the shown one";

    auto shortcut = entry("Tool", "", StartupLocation::StartupFolderUser, true);
    shortcut.registrationName = "Tool.lnk";
    EXPECT_EQ(startupRegistrationLabel(shortcut), "") << "the tooltip shows the shortcut's path already";

    auto packaged = entry("Spotify", "Spotify AB", StartupLocation::PackagedApp, false);
    packaged.registrationName = "Spotify";
    packaged.packageFamily = "SpotifyAB.SpotifyMusic_zpdnekdrzrea0";
    EXPECT_EQ(startupRegistrationLabel(packaged), "Startup task: SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify");
    packaged.packageFamily.clear();
    EXPECT_EQ(startupRegistrationLabel(packaged), "");
}

TEST(StartupViewTest, FilterMatchesTheRegisteredNameAndPolicyIsLabelled)
{
    const std::vector<Platform::StartupEntry> entries = {edgeEntry(), entry("Discord", "", StartupLocation::RunUser, true)};
    EXPECT_EQ(buildStartupRows(entries, "autolaunch", StartupColumn::Name, true), (std::vector<std::size_t>{0}));
    EXPECT_EQ(buildStartupRows(entries, "microsoft edge", StartupColumn::Name, true), (std::vector<std::size_t>{0}));

    auto managed = entry("Company Portal", "", StartupLocation::PackagedApp, true);
    managed.setByPolicy = true;
    EXPECT_EQ(startupEnabledLabel(managed), "Enabled by policy");
    managed.enabled = false;
    EXPECT_EQ(startupEnabledLabel(managed), "Disabled by policy");
}

TEST(StartupViewTest, OneProgramUnderTwoValuesSortsByTheValue)
{
    auto second = edgeEntry();
    second.registrationName = "MicrosoftEdgeAutoLaunch_111111";
    const std::vector<Platform::StartupEntry> entries = {edgeEntry(), second};
    EXPECT_EQ(buildStartupRows(entries, "", StartupColumn::Enabled, true), (std::vector<std::size_t>{1, 0}));
}

TEST_F(StartupViewRenderTest, TheNameTooltipShowsTheRegistryValue)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    publication.entries = {edgeEntry()};
    StartupViewState state;
    EXPECT_TRUE(captureFrame(publication, state).contains("Microsoft Edge"));
    const ImVec2 row = firstRowCentre(publication, state);
    ASSERT_GT(row.y, 0.0F);
    const std::string text = hoverUntil(publication, state, row, "Registry value:");
    EXPECT_TRUE(text.contains("Registry value: MicrosoftEdgeAutoLaunch_575A7C"));
}

// #1971: a RunOnce entry is shown by its value name; its program's name goes in the tooltip.

/// A RunOnce clean-up entry that runs cmd.exe, as the probe names it.
[[nodiscard]] Platform::StartupEntry cleanupEntry()
{
    auto cleanup = entry("Uninstall 24.1.0.1", "Microsoft Corporation", StartupLocation::RunOnceUser, true);
    cleanup.registrationName = "Uninstall 24.1.0.1";
    cleanup.programName = "Windows Command Processor";
    return cleanup;
}

TEST(StartupViewTest, ProgramNameLabelShowsOnlyWhenTheNameShownIsAnother)
{
    EXPECT_EQ(startupProgramNameLabel(cleanupEntry()), "Program name: Windows Command Processor");
    EXPECT_EQ(startupRegistrationLabel(cleanupEntry()), "") << "the value name is the name shown";

    auto edge = edgeEntry();
    edge.programName = edge.name;
    EXPECT_EQ(startupProgramNameLabel(edge), "") << "a Run entry is shown by its program's name already";
    EXPECT_EQ(startupProgramNameLabel(entry("Spotify", "Spotify AB", StartupLocation::PackagedApp, true)), "") << "no program name";
}

TEST(StartupViewTest, FilterMatchesARunOnceEntrysProgramName)
{
    const std::vector<Platform::StartupEntry> entries = {cleanupEntry(), entry("Discord", "", StartupLocation::RunUser, true)};
    EXPECT_EQ(buildStartupRows(entries, "command processor", StartupColumn::Name, true), (std::vector<std::size_t>{0}));
    EXPECT_EQ(buildStartupRows(entries, "uninstall", StartupColumn::Name, true), (std::vector<std::size_t>{0}));
}

TEST_F(StartupViewRenderTest, ARunOnceNameTooltipShowsTheProgramName)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    publication.entries = {cleanupEntry()};
    StartupViewState state;
    EXPECT_TRUE(captureFrame(publication, state).contains("Uninstall 24.1.0.1"));
    const ImVec2 row = firstRowCentre(publication, state);
    ASSERT_GT(row.y, 0.0F);
    const std::string text = hoverUntil(publication, state, row, "Program name:");
    EXPECT_TRUE(text.contains("Program name: Windows Command Processor"));
    EXPECT_FALSE(text.contains("Registry value:"));
}

TEST_F(StartupViewRenderTest, SelectionIsByTheRegisteredNameAndPackage)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    auto music = entry("Music", "", StartupLocation::PackagedApp, true);
    music.registrationName = "App";
    music.packageFamily = "Contoso.Music_abc";
    auto video = entry("Video", "", StartupLocation::PackagedApp, true);
    video.registrationName = "App"; // the same TaskId in another package
    video.packageFamily = "Contoso.Video_abc";
    publication.entries = {music, video};
    StartupActionsView actions(std::make_shared<IdleStartupActions>());
    StartupViewState state;
    state.selectedName = "App";
    state.selectedLocation = StartupLocation::PackagedApp;
    state.selectedFamily = "Contoso.Video_abc";
    const std::string text = captureFrame(publication, state, &actions);
    EXPECT_FALSE(text.contains("Select a startup app"));
    EXPECT_TRUE(text.contains("Video"));
    state.selectedFamily = "Contoso.Other_abc";
    EXPECT_TRUE(captureFrame(publication, state, &actions).contains("Select a startup app"));
}

// ========== Startup impact (#3073) ==========

/// makePublication() with each entry's impact: Discord High (2 processes), OneDrive None (disabled),
/// SecurityHealth Low, Stale Not measured (its program is missing).
[[nodiscard]] Domain::StartupPublication makeImpactPublication()
{
    auto publication = makePublication();
    publication.impacts = {
        {.cpuUs = 2'345'678, .diskBytes = 12'000'000, .processCount = 2, .rating = Domain::StartupImpact::High},
        {.rating = Domain::StartupImpact::None},
        {.cpuUs = 26'041, .diskBytes = 204'800, .processCount = 1, .rating = Domain::StartupImpact::Low},
        {},
    };
    publication.impactSignInUnixSeconds = 1792774334;
    return publication;
}

TEST(StartupViewTest, ImpactLabelsAreTaskManagersWords)
{
    EXPECT_EQ(startupImpactLabel(Domain::StartupImpact::High), "High");
    EXPECT_EQ(startupImpactLabel(Domain::StartupImpact::Medium), "Medium");
    EXPECT_EQ(startupImpactLabel(Domain::StartupImpact::Low), "Low");
    EXPECT_EQ(startupImpactLabel(Domain::StartupImpact::NotMeasured), "Not measured");
    EXPECT_EQ(startupImpactLabel(Domain::StartupImpact::None), "None");
}

TEST(StartupViewTest, ImpactTooltipGivesTheMeasuredFiguresOrWhyThereAreNone)
{
    const Domain::StartupEntryImpact high{
        .cpuUs = 2'345'678, .diskBytes = 12'000'000, .processCount = 2, .rating = Domain::StartupImpact::High};
    const std::string measured = startupImpactTooltip(high, 1792774334, "");
    EXPECT_TRUE(measured.starts_with("At the sign-in at 2026-10-2")) << measured; // local time
    EXPECT_TRUE(measured.contains("\nCPU time: 2")) << measured;
    EXPECT_TRUE(measured.contains(" ms\nDisk I/O: ")) << measured;
    EXPECT_TRUE(measured.contains("2 processes: the program and those it started")) << measured;
    EXPECT_TRUE(measured.contains("High: over 1 s of CPU or 3 MB of disk I/O. Low: under 300 ms of CPU and 300 KB of disk I/O."));

    const Domain::StartupEntryImpact one{.cpuUs = 1000, .diskBytes = 0, .processCount = 1, .rating = Domain::StartupImpact::Low};
    const std::string single = startupImpactTooltip(one, 0, "");
    EXPECT_TRUE(single.starts_with("At the last sign-in:")) << single;
    EXPECT_FALSE(single.contains("processes")) << "one process: nothing to add";

    EXPECT_EQ(startupImpactTooltip({.rating = Domain::StartupImpact::None}, 1792774334, ""),
              "Disabled: it doesn't start when you sign in.");
    EXPECT_EQ(startupImpactTooltip({}, 0, "Windows hasn't measured a sign-in for this account yet"),
              "Not measured: Windows hasn't measured a sign-in for this account yet.");
    EXPECT_EQ(startupImpactTooltip({}, 0, ""), "Not measured: Windows has no figures for its program at the last sign-in.");
    const std::string absent = startupImpactTooltip({}, 1792774334, "");
    EXPECT_TRUE(absent.starts_with("Not measured: its program didn't run during the measurement of the sign-in at 2026-10-2")) << absent;
}

TEST(StartupViewTest, ImpactRowsSortByRatingThenCpuTime)
{
    const auto publication = makeImpactPublication();
    // Ascending: None (OneDrive), Not measured (Stale), Low (SecurityHealth), High (Discord).
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Impact, true, publication.impacts),
              (std::vector<std::size_t>{1, 3, 2, 0}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Impact, false, publication.impacts),
              (std::vector<std::size_t>{0, 2, 3, 1}));

    // Within a rating, by CPU time, then disk I/O; an entry past the impacts is Not measured.
    const std::vector<Domain::StartupEntryImpact> impacts = {
        {.cpuUs = 5'000'000, .diskBytes = 0, .processCount = 1, .rating = Domain::StartupImpact::High},
        {.cpuUs = 2'000'000, .diskBytes = 9, .processCount = 1, .rating = Domain::StartupImpact::High},
        {.cpuUs = 2'000'000, .diskBytes = 1, .processCount = 1, .rating = Domain::StartupImpact::High},
    };
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Impact, false, impacts), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Impact, true, impacts), (std::vector<std::size_t>{3, 2, 1, 0}));
    // No impacts at all (a platform without them): every entry ties, in the model's name order.
    EXPECT_EQ(buildStartupRows(publication.entries, "", StartupColumn::Impact, true), (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST_F(StartupViewRenderTest, ImpactColumnShowsEachEntrysRating)
{
    const auto publication = makeImpactPublication();
    StartupViewState state;
    const std::string text = captureFrame(publication, state);
    EXPECT_TRUE(text.contains("Startup impact"));
    EXPECT_TRUE(text.contains("High"));
    EXPECT_TRUE(text.contains("Low"));
    EXPECT_TRUE(text.contains("None"));
    EXPECT_TRUE(text.contains("Not measured"));
}

TEST_F(StartupViewRenderTest, ImpactColumnIsLeftOutWithoutImpactData)
{
    // The Linux probe's capabilities: no impact data, so no column (and nothing drawn for it).
    const auto publication = makeImpactPublication();
    StartupViewState state;
    std::string text;
    static_cast<void>(runFrame(
        [&]
        {
            ImGui::LogToBuffer();
            const StartupViewContent content = renderStartupView(&publication, enumerateOnly(), state);
            text = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
            return content;
        }));
    EXPECT_FALSE(text.contains("Startup impact"));
    EXPECT_FALSE(text.contains("Not measured"));
    EXPECT_TRUE(text.contains("Discord"));
}

TEST_F(StartupViewRenderTest, HoveringTheImpactCellShowsTheMeasuredFigures)
{
    const auto publication = makeImpactPublication();
    StartupViewState state;
    // The default order lists Discord first: High, from two processes.
    const ImVec2 impact = firstRowCentre(publication, state, nullptr, static_cast<int>(StartupColumn::Impact));
    ASSERT_GT(impact.y, 0.0F);
    const std::string text = hoverUntil(publication, state, impact, "CPU time:");
    EXPECT_TRUE(text.contains("CPU time:")) << text;
    EXPECT_TRUE(text.contains("Disk I/O:"));
    EXPECT_TRUE(text.contains("2 processes"));
    EXPECT_FALSE(text.contains("Program: ")) << "the impact tooltip stands in for the entry's";

    // Elsewhere on the row, the entry's own tooltip.
    const ImVec2 name = firstRowCentre(publication, state);
    const std::string nameText = hoverUntil(publication, state, name, "Program: ");
    EXPECT_TRUE(nameText.contains("Program: ")) << nameText;
    EXPECT_FALSE(nameText.contains("CPU time:"));
}

} // namespace
} // namespace App
