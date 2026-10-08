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
