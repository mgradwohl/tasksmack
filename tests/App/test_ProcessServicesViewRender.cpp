/// @file test_ProcessServicesViewRender.cpp
/// @brief Process Details' Services section (#1880): the rows it builds from the process's service names
/// and the service list, and its real ImGui render, headless -- nothing drawn where the platform cannot
/// tell, nothing but the header while collapsed (and the list then not wanted), the names, display names
/// and states while open, the muted lines for none and for a list not read yet, and a name's link
/// reporting the service to open in the Services tab. The text it hands ImGui is captured through
/// ImGui's own logging (LogToBuffer()).

#include "App/Panels/ProcessServicesView.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceProbe.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

/// An svchost.exe (PID 1200) hosting DHCP and the event log, as the Services panel's model publishes it.
[[nodiscard]] Domain::ServicePublication svchostList()
{
    Domain::ServicePublication publication;
    publication.version = 3;
    publication.services = {service("Dhcp", "DHCP Client", ServiceState::Running, 1200),
                            service("EventLog", "Windows Event Log", ServiceState::StopPending, 1200)};
    publication.servicesByPid = Domain::indexServicesByPid(publication.services);
    return publication;
}

/// The svchost's service names, as its snapshot lists them.
[[nodiscard]] std::vector<std::string> svchostNames()
{
    return {"Dhcp", "EventLog"};
}

TEST(ProcessServicesViewTest, RowsTakeDisplayNamesAndStatesFromTheList)
{
    const Domain::ServicePublication publication = svchostList();
    const std::vector<std::string> names{"Dhcp", "EventLog", "ssh.service"};
    const std::vector<Detail::HostedServiceRow> expected{
        {.name = "Dhcp", .displayName = "DHCP Client", .state = ServiceState::Running, .listed = true},
        {.name = "EventLog", .displayName = "Windows Event Log", .state = ServiceState::StopPending, .listed = true},
        // Not in the list (a Linux unit while there is no systemd list): no display name, state unknown.
        {.name = "ssh.service", .displayName = "", .state = ServiceState::Unknown, .listed = false},
    };
    EXPECT_EQ(Detail::hostedServiceRows(names, &publication), expected);
    EXPECT_FALSE(Detail::hostedServiceRows(names, nullptr)[0].listed);
    EXPECT_TRUE(Detail::hostedServiceRows({}, &publication).empty());
}

class ProcessServicesViewRenderTest : public ::testing::Test
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

    struct Frame
    {
        std::string text;
        std::optional<std::string> clicked;
    };

    /// One frame drawing the section, open or collapsed, and what it drew and returned.
    static Frame renderFrame(ProcessServicesView& view,
                             bool hasServices,
                             const std::vector<std::string>& names,
                             const ProcessServicesView::ServiceList& list,
                             bool open = true)
    {
        Frame frame;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextItemOpen(open, ImGuiCond_Always);
        frame.clicked = view.render(hasServices, names, list);
        frame.text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return frame;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessServicesViewRenderTest, NothingWhereThePlatformCannotTell)
{
    ProcessServicesView view;
    const Domain::ServicePublication publication = svchostList();
    EXPECT_TRUE(renderFrame(view, false, svchostNames(), {.publication = &publication, .expected = true}).text.empty());
    EXPECT_FALSE(view.takeDrawnOpen());
}

TEST_F(ProcessServicesViewRenderTest, CollapsedShowsTheHeaderAloneAndWantsNoList)
{
    ProcessServicesView view;
    const Domain::ServicePublication publication = svchostList();
    const Frame frame = renderFrame(view, true, svchostNames(), {.publication = &publication, .expected = true}, /*open=*/false);
    EXPECT_TRUE(frame.text.contains("Services")) << frame.text;
    EXPECT_FALSE(frame.text.contains("DHCP Client")) << frame.text;
    EXPECT_FALSE(view.takeDrawnOpen());
}

TEST_F(ProcessServicesViewRenderTest, OpenListsEachServiceWithItsDisplayNameAndState)
{
    ProcessServicesView view;
    const Domain::ServicePublication publication = svchostList();
    const Frame frame = renderFrame(view, true, svchostNames(), {.publication = &publication, .expected = true});
    const std::array<const char*, 9> expected{
        "Name", "Display name", "State", "Dhcp", "DHCP Client", "Running", "EventLog", "Windows Event Log", "Stopping"};
    std::size_t index = 0;
    while (index < expected.size())
    {
        EXPECT_TRUE(frame.text.contains(expected[index])) << expected[index] << " in " << frame.text;
        ++index;
    }
    EXPECT_FALSE(frame.clicked.has_value());
    EXPECT_TRUE(view.takeDrawnOpen());
    EXPECT_FALSE(view.takeDrawnOpen()); // taken once per frame drawn open
}

TEST_F(ProcessServicesViewRenderTest, NoneAndNotReadYetAreToldApart)
{
    ProcessServicesView view;
    const Domain::ServicePublication publication = svchostList();
    // Windows, list read: this process hosts none.
    EXPECT_TRUE(renderFrame(view, true, {}, {.publication = &publication, .expected = true}).text.contains("No services"));
    // Windows, the list not read (or not current) yet: the names are not known, not none.
    EXPECT_TRUE(renderFrame(view, true, {}, {.publication = nullptr, .expected = true}).text.contains("Reading services..."));
    // Linux: no list is expected; no unit is none.
    EXPECT_TRUE(renderFrame(view, true, {}, {.publication = nullptr, .expected = false}).text.contains("No services"));
}

TEST_F(ProcessServicesViewRenderTest, AUnitTheListLacksIsShownWithoutALink)
{
    ProcessServicesView view;
    const Frame frame = renderFrame(view, true, {"ssh.service"}, {.publication = nullptr, .expected = false});
    EXPECT_TRUE(frame.text.contains("ssh.service")) << frame.text;
    EXPECT_TRUE(frame.text.contains("\xE2\x80\x94")) << frame.text; // state not known
}

TEST_F(ProcessServicesViewRenderTest, ANamesLinkReportsTheServiceToShow)
{
    ProcessServicesView view;
    const Domain::ServicePublication publication = svchostList();
    const ProcessServicesView::ServiceList list{.publication = &publication, .expected = true};
    static_cast<void>(renderFrame(view, true, svchostNames(), list));

    // EventLog's link: row 1 of the section's table, keyed by its row (PushID) and name (TextLink).
    const ImGuiWindow* window = ImGui::FindWindowByName("Shell");
    ASSERT_NE(window, nullptr);
    const ImGuiID tableId = ImHashStr("##ServicesTable", 0, window->ID);
    const int row = 1;
    const ImGuiID rowId = ImHashData(&row, sizeof(row), tableId);
    ImGui::ActivateItemByID(ImHashStr("EventLog", 0, rowId));

    std::optional<std::string> clicked;
    int frames = 0;
    while (!clicked.has_value() && frames < 4)
    {
        clicked = renderFrame(view, true, svchostNames(), list).clicked;
        ++frames;
    }
    EXPECT_EQ(clicked.value_or(""), "EventLog");
}

} // namespace
} // namespace App
