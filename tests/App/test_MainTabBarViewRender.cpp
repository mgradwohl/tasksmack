/// @file test_MainTabBarViewRender.cpp
/// @brief The main tab bar, headless (#1547): the first tab starts selected, a tab asked for is
/// selected that frame and stays selected after the request ends, a click selects another, and the
/// selection follows a tab's "###" ID rather than its changing text. The bar keeps the content gutter
/// on its right as on its left, and measureWidth() is the width at which every tab fits whole (#1939).

#include "App/MainTabBarView.h"
#include "App/ShellMetrics.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImHashStr(), ActivateItemByID(): pressing a tab

#include <array>
#include <cmath>
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

    /// One frame of the bar in a shell window `width` wide (the whole display by default), with no
    /// window border or padding, as ShellLayer draws it.
    MainTabBarView::Result frame(float width = 0.0F)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(width > 0.0F ? width : ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);
        const MainTabBarView::Result result = MainTabBarView::render(m_Tabs);
        m_BarId = ImGui::GetID("##MainTabBar");
        std::array<const char*, 3> labels{};
        for (std::size_t i = 0; i < m_Tabs.size(); ++i)
        {
            labels[i] = m_Tabs[i].label;
        }
        m_MeasuredWidth = MainTabBarView::measureWidth(labels);
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
    float m_MeasuredWidth = 0.0F; ///< measureWidth() of the tabs' labels, last frame

    /// The bar ImGui laid out last frame.
    [[nodiscard]] const ImGuiTabBar& bar() const
    {
        const ImGuiTabBar* tabBar = ImGui::GetCurrentContext()->TabBars.GetByKey(m_BarId);
        EXPECT_NE(tabBar, nullptr);
        return *tabBar;
    }

    /// Where the last tab ends, in screen space.
    [[nodiscard]] float lastTabRight() const
    {
        const ImGuiTabBar& tabBar = bar();
        const ImGuiTabItem& last = tabBar.Tabs[tabBar.Tabs.Size - 1];
        return tabBar.BarRect.Min.x + last.Offset + last.Width;
    }

    /// The gutter the bar keeps either side.
    [[nodiscard]] static float inset()
    {
        return ShellMetrics::CONTENT_PADDING_H * UI::Theme::get().styleScale();
    }

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

// At the width measureWidth() gives, every tab is drawn whole and the last ends the gutter short of
// the window's right edge: the window's minimum width covers this, so the bar fits there (#1939).
TEST_F(MainTabBarViewRenderTest, AtTheMeasuredWidthEveryTabFitsWholeInsideTheGutter)
{
    static_cast<void>(frame());
    const float width = std::ceil(m_MeasuredWidth); // A window is whole pixels; the minimum rounds up
    ASSERT_GT(width, 0.0F);
    static_cast<void>(frame(width));
    static_cast<void>(frame(width));

    const ImGuiTabBar& tabBar = bar();
    ASSERT_EQ(tabBar.Tabs.Size, static_cast<int>(m_Tabs.size()));
    for (const ImGuiTabItem& tab : tabBar.Tabs)
    {
        EXPECT_FLOAT_EQ(tab.Width, tab.ContentWidth); // Not shrunk: every label is shown whole
    }
    EXPECT_FLOAT_EQ(tabBar.BarRect.Min.x, inset());
    EXPECT_LE(lastTabRight(), width - inset() + 0.01F);
    EXPECT_NEAR(lastTabRight(), width - inset(), 1.0F); // and no wider than it has to be
}

// Narrower than that -- a minimum held to a small display's usable width -- ImGui shrinks the tabs,
// and the last still ends inside the gutter instead of flush against the window's edge (#1939).
TEST_F(MainTabBarViewRenderTest, NarrowerTheTabsShrinkInsideTheGutter)
{
    // Labels long enough that 40px off them leaves every tab above TabMinWidthShrink, where ImGui
    // would add scroll buttons instead (ImGuiTabBarFlags_FittingPolicyMixed).
    m_Tabs[0].label = "Overview of this machine###OverviewTab";
    m_Tabs[1].label = "Processes running now###ProcessesTab";
    m_Tabs[2].label = "Select a process###DetailsTab";
    static_cast<void>(frame());
    const float width = std::round(m_MeasuredWidth - 40.0F);
    static_cast<void>(frame(width));
    static_cast<void>(frame(width));

    EXPECT_FLOAT_EQ(bar().BarRect.Max.x, width - inset());
    EXPECT_LE(lastTabRight(), width - inset() + 0.01F);
    EXPECT_GT(lastTabRight(), width - inset() - 4.0F); // The tabs fill the bar, less ImGui truncating each width
}

// The measured width follows the labels: a longer one widens it by the difference.
TEST_F(MainTabBarViewRenderTest, TheMeasuredWidthFollowsTheLabels)
{
    static_cast<void>(frame());
    const float shortWidth = m_MeasuredWidth;
    m_Tabs[2].label = "a much longer process name###DetailsTab";
    static_cast<void>(frame());
    ImGui::NewFrame(); // CalcTextSize() needs a frame's font
    const float extra = ImGui::CalcTextSize("a much longer process name").x - ImGui::CalcTextSize("bash").x;
    ImGui::EndFrame();
    EXPECT_NEAR(m_MeasuredWidth - shortWidth, extra, 0.01F);
}

} // namespace
} // namespace App
