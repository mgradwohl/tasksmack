/// @file test_StartupViewRender.cpp
/// @brief The Startup tab's table (#801), headless: the unsupported message (the Linux stub's
/// capabilities), the loading state, rows for a publication (a missing target among them), the
/// filter, and the pure row order and labels.

#include "App/Panels/StartupActionsView.h"
#include "App/Panels/StartupView.h"
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

    /// The centre of the table's first row's Name cell, from a frame that drew @p publication.
    static ImVec2
    firstRowCentre(const Domain::StartupPublication& publication, StartupViewState& state, StartupActionsView* actions = nullptr)
    {
        ImVec2 centre(-1.0F, -1.0F);
        static_cast<void>(runFrame(
            [&]
            {
                const StartupViewContent content = renderStartupView(&publication, supported(), state, actions);
                if (const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##StartupTable")); table != nullptr)
                {
                    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                    centre = ImVec2((table->Columns[0].MinX + table->Columns[0].MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * 1.5F));
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
}

// The Windows-only Startup tab's remaining branches (#1395, Windows half).
TEST(StartupViewTest, EveryLocationHasItsLabelAndCommandSorts)
{
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunUser), "Registry: HKCU Run");
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunMachine), "Registry: HKLM Run");
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunOnceUser), "Registry: HKCU RunOnce");
    EXPECT_EQ(startupLocationLabel(StartupLocation::RunOnceMachine), "Registry: HKLM RunOnce");
    EXPECT_EQ(startupLocationLabel(StartupLocation::StartupFolderCommon), "Startup folder (all users)");

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

} // namespace
} // namespace App
