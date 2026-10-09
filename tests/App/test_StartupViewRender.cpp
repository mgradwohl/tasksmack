/// @file test_StartupViewRender.cpp
/// @brief The Startup tab's table (#801), headless: the unsupported message (the Linux stub's
/// capabilities), the loading state, rows for a publication (a missing target among them), the
/// filter, and the pure row order and labels.

#include "App/Panels/StartupView.h"
#include "Domain/StartupModel.h"
#include "Platform/IStartupProbe.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <functional>
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

    // Publisher matches too, case-insensitively.
    state.filter = "MICROSOFT";
    static_cast<void>(runFrame([&] { return renderStartupView(&publication, supported(), state); }));
    EXPECT_EQ(state.rows, (std::vector<std::size_t>{1, 2}));

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

} // namespace
} // namespace App
