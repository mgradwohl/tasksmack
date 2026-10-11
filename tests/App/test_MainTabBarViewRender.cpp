/// @file test_MainTabBarViewRender.cpp
/// @brief The main tab bar, headless (#1547): the first tab starts selected, a tab asked for is
/// selected that frame and stays selected after the request ends, a click selects another, and the
/// selection follows a tab's "###" ID rather than its changing text.

#include "App/MainTabBarView.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImHashStr(), ActivateItemByID(): pressing a tab

#include <array>
#include <cstddef>
#include <optional>

namespace App
{
namespace
{

class MainTabBarViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the font atlas
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    MainTabBarView::Result frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const MainTabBarView::Result result = MainTabBarView::render(m_Tabs);
        m_BarId = ImGui::GetID("##MainTabBar");
        ImGui::End();
        ImGui::Render();
        for (MainTabBarView::Tab& tab : m_Tabs)
        {
            tab.selectRequested = false; // A request is for one frame, as ShellLayer makes it
        }
        return result;
    }

    std::array<MainTabBarView::Tab, 3> m_Tabs{{
        {.label = "Overview###OverviewTab", .selectRequested = false},
        {.label = "Processes###ProcessesTab", .selectRequested = false},
        {.label = "bash###DetailsTab", .selectRequested = false},
    }};
    ImGuiID m_BarId = 0;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(MainTabBarViewRenderTest, TheFirstTabStartsSelected)
{
    const MainTabBarView::Result result = frame();
    EXPECT_TRUE(result.drawn);
    EXPECT_EQ(result.selected, std::optional<std::size_t>(0));
}

TEST_F(MainTabBarViewRenderTest, ARequestedTabIsSelectedAndStaysSelected)
{
    static_cast<void>(frame());
    m_Tabs[2].selectRequested = true; // ShowProcessDetailsEvent, or TASKSMACK_TAB at startup
    static_cast<void>(frame());
    EXPECT_EQ(frame().selected, std::optional<std::size_t>(2));
    EXPECT_EQ(frame().selected, std::optional<std::size_t>(2)); // after the request has ended
}

TEST_F(MainTabBarViewRenderTest, AClickSelectsATab)
{
    static_cast<void>(frame());
    ImGui::ActivateItemByID(ImHashStr("###ProcessesTab", 0, m_BarId));
    static_cast<void>(frame());
    EXPECT_EQ(frame().selected, std::optional<std::size_t>(1));
}

TEST_F(MainTabBarViewRenderTest, TheSelectionFollowsTheTabIdNotItsText)
{
    m_Tabs[2].selectRequested = true;
    static_cast<void>(frame());
    static_cast<void>(frame());
    // The selected process exec'd: the Details tab's text changes, its "###" ID doesn't (#1140).
    m_Tabs[2].label = "python3###DetailsTab";
    EXPECT_EQ(frame().selected, std::optional<std::size_t>(2));
}

} // namespace
} // namespace App
