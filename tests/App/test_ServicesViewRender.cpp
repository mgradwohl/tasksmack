/// @file test_ServicesViewRender.cpp
/// @brief The Services tab's table (#800), headless: the unsupported message (the Linux stub's
/// capabilities), the loading state, rows for a publication, the filter, and the pure row order.

#include "App/Panels/ServicesView.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceProbe.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <functional>
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

} // namespace
} // namespace App
