/// @file test_ServicesViewRender.cpp
/// @brief The Services tab's table (#800), headless: the unsupported message (the Linux stub's
/// capabilities), the loading state, rows for a publication, the filter, and the pure row order.

#include "App/Panels/ServiceActionsView.h"
#include "App/Panels/ServicesView.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer, TableFindByID()

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

using Platform::ServiceState;

[[nodiscard]] Platform::ServiceInfo service(const char* name, const char* displayName, ServiceState state, std::uint32_t pid)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = displayName;
    info.state = state;
    info.pid = pid;
    return info;
}

/// Capabilities of a probe that can list services.
[[nodiscard]] Platform::ServiceCapabilities enumerable()
{
    Platform::ServiceCapabilities capabilities;
    capabilities.canEnumerate = true;
    return capabilities;
}

[[nodiscard]] Domain::ServicePublication makePublication()
{
    Domain::ServicePublication publication;
    publication.version = 3;
    publication.services = {
        service("EventLog", "Windows Event Log", ServiceState::Running, 1400),
        service("Spooler", "Print Spooler", ServiceState::Stopped, 0),
        service("Winmgmt", "Windows Management Instrumentation", ServiceState::Running, 900),
    };
    return publication;
}

/// Elevated actions that are never run here: the tests only draw the bar and the row menus.
class IdleServiceActions final : public Platform::IServiceActions
{
  public:
    [[nodiscard]] Platform::ServiceActionCapabilities capabilities() const override
    {
        return {.canStart = true, .canStop = true, .canRestart = true, .canSetStartType = true, .elevated = true};
    }
    [[nodiscard]] Platform::ServiceActionResult start(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return Platform::ServiceActionResult::succeeded();
    }
    [[nodiscard]] Platform::ServiceActionResult stop(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return Platform::ServiceActionResult::succeeded();
    }
    [[nodiscard]] Platform::ServiceActionResult restart(std::string_view /*name*/, const std::stop_token& /*stopToken*/) override
    {
        return Platform::ServiceActionResult::succeeded();
    }
    [[nodiscard]] Platform::ServiceActionResult setStartType(std::string_view /*name*/, Platform::ServiceStartType /*type*/) override
    {
        return Platform::ServiceActionResult::succeeded();
    }
};

class ServicesViewRenderTest : public ::testing::Test
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

    static ServicesViewContent runFrame(const std::function<ServicesViewContent()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Services", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const ServicesViewContent content = body();
        ImGui::End();
        ImGui::Render();
        return content;
    }

    /// One frame of the view, returning every text it drew (tooltips included).
    static std::string
    captureFrame(const Domain::ServicePublication& publication, ServicesViewState& state, ServiceActionsView* actions = nullptr)
    {
        std::string captured;
        static_cast<void>(runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                const ServicesViewContent content = renderServicesView(&publication, enumerable(), state, actions);
                captured = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
                return content;
            }));
        return captured;
    }

    /// The centre of the table's first row's Name cell, from a frame that drew @p publication.
    static ImVec2 firstRowCentre(const Domain::ServicePublication& publication, ServicesViewState& state)
    {
        ImVec2 centre(-1.0F, -1.0F);
        static_cast<void>(runFrame(
            [&]
            {
                const ServicesViewContent content = renderServicesView(&publication, enumerable(), state);
                if (const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##ServicesTable")); table != nullptr)
                {
                    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                    centre = ImVec2((table->Columns[0].MinX + table->Columns[0].MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * 1.5F));
                }
                return content;
            }));
        return centre;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ServicesViewRenderTest, UnsupportedPlatformShowsTheMessage)
{
    // The Linux factory's UnsupportedServiceProbe reports these capabilities.
    const Platform::ServiceCapabilities unsupported = Platform::UnsupportedServiceProbe{}.capabilities();
    ServicesViewState state;
    EXPECT_EQ(runFrame([&] { return renderServicesView(nullptr, unsupported, state); }), ServicesViewContent::Unsupported);
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
    EXPECT_TRUE(state.unavailableHeading.contains("Services aren't available on this platform yet"));
}

TEST_F(ServicesViewRenderTest, DeniedServiceManagerShowsItsReason)
{
    Platform::ServiceCapabilities denied;
    denied.unavailableReason = "Access to the Service Control Manager was denied";
    ServicesViewState state;
    EXPECT_EQ(runFrame([&] { return renderServicesView(nullptr, denied, state); }), ServicesViewContent::Unsupported);
    EXPECT_TRUE(state.unavailableHeading.contains("was denied"));
}

TEST_F(ServicesViewRenderTest, NoSampleYetShowsLoading)
{
    ServicesViewState state;
    const Domain::ServicePublication empty;
    EXPECT_EQ(runFrame([&] { return renderServicesView(&empty, enumerable(), state); }), ServicesViewContent::Loading);
}

TEST_F(ServicesViewRenderTest, TableShowsRowsAndFilters)
{
    const auto publication = makePublication();
    ServicesViewState state;
    EXPECT_EQ(runFrame([&] { return renderServicesView(&publication, enumerable(), state); }), ServicesViewContent::Table);
    EXPECT_EQ(state.rows.size(), 3U);
    EXPECT_EQ(state.rowsVersion, 3U);

    state.filter = "WINDOWS";
    static_cast<void>(runFrame([&] { return renderServicesView(&publication, enumerable(), state); }));
    EXPECT_EQ(state.rows, (std::vector<std::size_t>{0, 2}));
}

TEST_F(ServicesViewRenderTest, AFailedReadKeepsTheLastRowsMarkedOutOfDate)
{
    auto publication = makePublication();
    publication.stale = true;
    publication.failureReason = "Access to the Service Control Manager was denied";
    ServicesViewState state;
    EXPECT_EQ(runFrame([&] { return renderServicesView(&publication, enumerable(), state); }), ServicesViewContent::StaleTable);
    EXPECT_EQ(state.rows.size(), 3U); // the last good rows are still listed
}

TEST_F(ServicesViewRenderTest, AFailedReadWithNoRowsShowsTheReason)
{
    Domain::ServicePublication publication;
    publication.version = 1;
    publication.stale = true;
    publication.failureReason = "The Service Control Manager could not list the services (error 1722)";
    ServicesViewState state;
    EXPECT_EQ(runFrame([&] { return renderServicesView(&publication, enumerable(), state); }), ServicesViewContent::ReadFailed);
    EXPECT_TRUE(state.rows.empty());
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
}

TEST(ServicesViewTest, RowsSortByColumnAndDirection)
{
    const auto publication = makePublication();
    EXPECT_EQ(buildServiceRows(publication.services, "", ServiceColumn::Pid, true), (std::vector<std::size_t>{1, 2, 0}));
    EXPECT_EQ(buildServiceRows(publication.services, "", ServiceColumn::DisplayName, false), (std::vector<std::size_t>{2, 0, 1}));
    EXPECT_EQ(buildServiceRows(publication.services, "spool", ServiceColumn::Name, true), (std::vector<std::size_t>{1}));
}

TEST_F(ServicesViewRenderTest, DefaultSortIsStateThenDisplayName)
{
    // #1599: with no header clicked, the table is sorted by State -- running services A to Z by display
    // name, then the stopped ones -- and the table's own default sort spec agrees, so it stays.
    const auto publication = makePublication();
    ServicesViewState state;
    EXPECT_EQ(state.sortColumn, ServiceColumn::State);
    EXPECT_TRUE(state.ascending);
    static_cast<void>(runFrame([&] { return renderServicesView(&publication, enumerable(), state); }));
    static_cast<void>(runFrame([&] { return renderServicesView(&publication, enumerable(), state); }));
    EXPECT_EQ(state.sortColumn, ServiceColumn::State);
    EXPECT_TRUE(state.ascending);
    EXPECT_EQ(state.rows, (std::vector<std::size_t>{0, 2, 1}));
}

TEST(ServicesViewTest, StateRanksRunningFirstAndUnknownLast)
{
    using enum ServiceState;
    const std::vector<ServiceState> order = {Running, StartPending, ContinuePending, PausePending, Paused, StopPending, Stopped, Unknown};
    for (std::size_t i = 0; i + 1 < order.size(); ++i)
    {
        EXPECT_LT(serviceStateSortRank(order[i]), serviceStateSortRank(order[i + 1])) << serviceStateLabel(order[i]);
    }
}

TEST(ServicesViewTest, StateSortCoversEveryStateThenDisplayName)
{
    // Listed in the opposite of the expected order, with names that would sort differently.
    const std::vector<Platform::ServiceInfo> services = {
        service("a", "Alpha", ServiceState::Unknown, 0),
        service("b", "bravo", ServiceState::Stopped, 0),
        service("c", "Charlie", ServiceState::StopPending, 10),
        service("d", "delta", ServiceState::Paused, 11),
        service("e", "Echo", ServiceState::PausePending, 12),
        service("f", "foxtrot", ServiceState::ContinuePending, 13),
        service("g", "Golf", ServiceState::StartPending, 14),
        service("h", "zulu", ServiceState::Running, 15),
        service("i", "Hotel", ServiceState::Running, 16),
        service("j", "apple", ServiceState::Stopped, 0),
    };
    EXPECT_EQ(buildServiceRows(services, "", ServiceColumn::State, true), (std::vector<std::size_t>{8, 7, 6, 5, 4, 3, 2, 9, 1, 0}));
    // Descending reverses the states only: the names stay A to Z within each.
    EXPECT_EQ(buildServiceRows(services, "", ServiceColumn::State, false), (std::vector<std::size_t>{0, 9, 1, 2, 3, 4, 5, 6, 8, 7}));
}

TEST(ServicesViewTest, StateSortNamesIgnoreCaseAndBreakTiesDeterministically)
{
    const std::vector<Platform::ServiceInfo> services = {
        service("svcB", "Shared Name", ServiceState::Running, 1),
        service("Zed", "", ServiceState::Running, 2), // no display name: listed by its service name
        service("svcA", "shared name", ServiceState::Running, 3),
        service("Mid", "MIDDLE", ServiceState::Running, 4),
        service("low", "aardvark", ServiceState::Stopped, 0),
        service("svca", "Shared Name", ServiceState::Running, 5),
    };
    // Display names compared without case; equal ones by service name (case ignored, then exact:
    // "svcA" before "svca"), so the order never depends on the order the services arrive in.
    const std::vector<std::size_t> expected = {3, 2, 5, 0, 1, 4};
    EXPECT_EQ(buildServiceRows(services, "", ServiceColumn::State, true), expected);
    const std::vector<Platform::ServiceInfo> reversed(services.rbegin(), services.rend());
    EXPECT_EQ(buildServiceRows(reversed, "", ServiceColumn::State, true), (std::vector<std::size_t>{2, 3, 0, 5, 4, 1}));
}

TEST(ServicesViewTest, LabelsAndColours)
{
    EXPECT_EQ(serviceStateLabel(ServiceState::StartPending), "Starting");
    EXPECT_EQ(serviceStartTypeLabel(Platform::ServiceStartType::AutomaticDelayed), "Automatic (delayed)");
    UI::ColorScheme scheme{};
    scheme.statusRunning = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    EXPECT_FLOAT_EQ(serviceStateColor(ServiceState::Running, scheme).y, 1.0F);
}

// The Windows-only Services tab's remaining branches (#1395, Windows half).
TEST(ServicesViewTest, EveryStateStartTypeAndColourHasItsLabel)
{
    using enum ServiceState;
    EXPECT_EQ(serviceStateLabel(Stopped), "Stopped");
    EXPECT_EQ(serviceStateLabel(StopPending), "Stopping");
    EXPECT_EQ(serviceStateLabel(ContinuePending), "Resuming");
    EXPECT_EQ(serviceStateLabel(PausePending), "Pausing");
    EXPECT_EQ(serviceStateLabel(Paused), "Paused");
    EXPECT_EQ(serviceStateLabel(Unknown), "Unknown");

    using Platform::ServiceStartType;
    EXPECT_EQ(serviceStartTypeLabel(ServiceStartType::Automatic), "Automatic");
    EXPECT_EQ(serviceStartTypeLabel(ServiceStartType::Disabled), "Disabled");
    EXPECT_EQ(serviceStartTypeLabel(ServiceStartType::Boot), "Boot");
    EXPECT_EQ(serviceStartTypeLabel(ServiceStartType::System), "System");
    EXPECT_EQ(serviceStartTypeLabel(ServiceStartType::Unknown), "");

    UI::ColorScheme scheme{};
    scheme.statusDiskSleep = ImVec4(0.1F, 0.0F, 0.0F, 1.0F);
    scheme.statusStopped = ImVec4(0.2F, 0.0F, 0.0F, 1.0F);
    scheme.statusSleeping = ImVec4(0.3F, 0.0F, 0.0F, 1.0F);
    for (const ServiceState pending : {StartPending, StopPending, ContinuePending, PausePending})
    {
        EXPECT_FLOAT_EQ(serviceStateColor(pending, scheme).x, 0.1F) << serviceStateLabel(pending);
    }
    EXPECT_FLOAT_EQ(serviceStateColor(Paused, scheme).x, 0.2F);
    EXPECT_FLOAT_EQ(serviceStateColor(Stopped, scheme).x, 0.3F);
    EXPECT_FLOAT_EQ(serviceStateColor(Unknown, scheme).x, 0.3F);
}

TEST(ServicesViewTest, RowsSortByStartTypeAndAccount)
{
    auto publication = makePublication();
    publication.services[0].startType = Platform::ServiceStartType::Manual;
    publication.services[1].startType = Platform::ServiceStartType::Automatic;
    publication.services[2].startType = Platform::ServiceStartType::Disabled;
    publication.services[0].account = "LocalSystem";
    publication.services[1].account = "localservice";
    publication.services[2].account = "NetworkService";
    EXPECT_EQ(buildServiceRows(publication.services, "", ServiceColumn::StartType, true), (std::vector<std::size_t>{1, 2, 0}));
    EXPECT_EQ(buildServiceRows(publication.services, "", ServiceColumn::Account, true), (std::vector<std::size_t>{1, 0, 2}));
    EXPECT_EQ(buildServiceRows(publication.services, "", ServiceColumn::Account, false), (std::vector<std::size_t>{2, 0, 1}));
}

TEST_F(ServicesViewRenderTest, HoveringARowShowsItsDetails)
{
    Domain::ServicePublication publication;
    publication.version = 1;
    publication.services = {service("Spooler", "Print Spooler", ServiceState::Running, 2200)};
    publication.services[0].description = "Queues print jobs";
    publication.services[0].binaryPath = R"(C:\Windows\System32\spoolsv.exe)";
    publication.services[0].group = "print";
    publication.services[0].serviceType = "Own process";
    ServicesViewState state;
    const ImVec2 row = firstRowCentre(publication, state);
    ASSERT_GT(row.y, 0.0F);

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(row.x, row.y);
    io.DeltaTime = 0.25F; // past the tooltip's hover delay in a few frames
    std::string text;
    for (int i = 0; i < 6 && !text.contains("Queues print jobs"); ++i)
    {
        text = captureFrame(publication, state);
    }
    EXPECT_TRUE(text.contains("Queues print jobs"));
    EXPECT_TRUE(text.contains(R"(Command: C:\Windows\System32\spoolsv.exe)"));
    EXPECT_TRUE(text.contains("Group: print"));
    EXPECT_TRUE(text.contains("Type: Own process"));

    // A right click selects the row, as the row menu acts on it.
    EXPECT_TRUE(state.selectedName.empty());
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    static_cast<void>(captureFrame(publication, state));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    static_cast<void>(captureFrame(publication, state));
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    EXPECT_EQ(state.selectedName, "Spooler");
}

TEST_F(ServicesViewRenderTest, ActionBarActsOnTheSelectedServiceByName)
{
    const auto publication = makePublication();
    ServiceActionsView actions(std::make_shared<IdleServiceActions>());
    ServicesViewState state;
    EXPECT_TRUE(captureFrame(publication, state, &actions).contains("Select a service"));

    state.selectedName = "Spooler";
    EXPECT_FALSE(captureFrame(publication, state, &actions).contains("Select a service"));

    // A selection the latest sample no longer lists acts on nothing.
    state.selectedName = "RemovedService";
    EXPECT_TRUE(captureFrame(publication, state, &actions).contains("Select a service"));
}

} // namespace
} // namespace App
