/// @file test_MainTabSelectionRender.cpp
/// @brief TASKSMACK_TAB's selection through a real ImGui tab bar, headless (#1575): the tab is found by
/// its registered id wherever the bar draws it, and the request stays until that tab reports selected.
/// The loop below has the shape of ShellLayer::renderTabBar(): SetSelected on the wanted tab, and each
/// BeginTabItem() result reported back to the request.

#include "App/SelectOverride.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using App::SelectOverride::PendingMainTab;
using App::SelectOverride::resolveMainTab;
using App::SelectOverride::TabInfo;

constexpr float WINDOW_SIZE = 1200.0F;

/// A tab as the tab bar draws it: its registered id and its ImGui label (a fixed "###" ID, as the shell's).
struct DrawnTab
{
    std::string_view id;
    const char* label;
};

/// The shell's registration order (ShellLayer's PanelTabs).
const std::vector<TabInfo>& registered()
{
    static const std::vector<TabInfo> tabs{
        {.id = "SystemOverview", .text = "MYHOST"},
        {.id = "Processes", .text = "Processes"},
        {.id = "ProcessDetails", .text = "Select a process"},
        {.id = "Services", .text = "Services"},
        {.id = "Startup", .text = "Startup"},
    };
    return tabs;
}

constexpr DrawnTab SYSTEM{.id = "SystemOverview", .label = "MYHOST###SystemTab"};
constexpr DrawnTab PROCESSES{.id = "Processes", .label = "Processes###ProcessesTab"};
constexpr DrawnTab DETAILS{.id = "ProcessDetails", .label = "Select a process###ProcessDetailsTab"};
constexpr DrawnTab EXTRA{.id = "Extra", .label = "Extra###ExtraTab"};
constexpr DrawnTab SERVICES{.id = "Services", .label = "Services###ServicesTab"};
constexpr DrawnTab STARTUP{.id = "Startup", .label = "Startup###StartupTab"};

class MainTabSelectionRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(WINDOW_SIZE, WINDOW_SIZE);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of the main tab bar drawing @p order; returns the id of the tab BeginTabItem() reported
    /// selected (empty when none did).
    static std::string runFrame(PendingMainTab& request, const std::vector<DrawnTab>& order)
    {
        std::string selectedId;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_SIZE, WINDOW_SIZE));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::BeginTabBar("##MainTabBar"))
        {
            for (const DrawnTab& tab : order)
            {
                ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
                if (request.wantsSelected(tab.id))
                {
                    flags |= ImGuiTabItemFlags_SetSelected;
                }
                const bool selected = ImGui::BeginTabItem(tab.label, nullptr, flags);
                request.onTabSubmitted(tab.id, selected);
                if (selected)
                {
                    selectedId = tab.id;
                    ImGui::EndTabItem();
                }
            }
            static_cast<void>(request.onFrameEnd());
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();
        return selectedId;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

// Registered System, Processes, Details, Services, Startup; drawn with an extra tab inserted before
// Services, so Startup's registered index (4) is Services' place on the bar and Services' (3) is the extra
// tab's: the issue's swap, had the request been an index. By id, the requested tab opens.
TEST_F(MainTabSelectionRenderTest, SelectsTheRequestedIdWhenTheBarDrawsTabsInAnotherOrder)
{
    const std::vector<DrawnTab> drawn{SYSTEM, PROCESSES, DETAILS, EXTRA, SERVICES, STARTUP};
    for (const char* name : {"startup", "services", "Processes", "details", "machine"})
    {
        SCOPED_TRACE(name);
        const auto choice = resolveMainTab(name, registered());
        ASSERT_TRUE(choice.id.has_value());
        PendingMainTab request(choice.id);

        std::string selected;
        for (int frame = 0; frame < 3; ++frame)
        {
            selected = runFrame(request, drawn);
        }
        const std::string wanted = choice.id.value_or("");
        EXPECT_EQ(selected, wanted);
        EXPECT_FALSE(request.pending());

        // And it stays there once the request is done.
        EXPECT_EQ(runFrame(request, drawn), wanted);
    }
}

// ImGui applies SetSelected a frame late: on the first frame the bar shows its first tab, and the request
// must still be waiting then.
TEST_F(MainTabSelectionRenderTest, RequestStaysPendingUntilItsTabReportsSelected)
{
    PendingMainTab request(resolveMainTab("startup", registered()).id);
    const std::vector<DrawnTab> drawn{SYSTEM, PROCESSES, DETAILS, SERVICES, STARTUP};

    const std::string first = runFrame(request, drawn);
    EXPECT_NE(first, "Startup");
    EXPECT_TRUE(request.pending());

    EXPECT_EQ(runFrame(request, drawn), "Startup");
    EXPECT_FALSE(request.pending());
}

// A tab bar that does not draw the requested tab yet (as when it is submitted later) keeps the request
// waiting, whatever else is selected; it opens on the frames after the tab is drawn.
TEST_F(MainTabSelectionRenderTest, RequestWaitsForItsTabToBeDrawn)
{
    PendingMainTab request(resolveMainTab("startup", registered()).id);
    const std::vector<DrawnTab> withoutStartup{SYSTEM, PROCESSES, DETAILS, SERVICES};
    for (int frame = 0; frame < 5; ++frame)
    {
        EXPECT_NE(runFrame(request, withoutStartup), "Startup") << frame;
        EXPECT_TRUE(request.pending()) << frame;
    }

    const std::vector<DrawnTab> withStartup{SYSTEM, PROCESSES, DETAILS, SERVICES, STARTUP};
    std::string selected;
    for (int frame = 0; frame < 3 && request.pending(); ++frame)
    {
        selected = runFrame(request, withStartup);
    }
    EXPECT_EQ(selected, "Startup");
    EXPECT_FALSE(request.pending());
}

} // namespace
