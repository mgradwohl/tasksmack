/// @file test_StatusBarViewRender.cpp
/// @brief The status bar, headless (#1547): the process count and update interval, the limited-data
/// and blocked-network icons only when they apply, Settings and Help only when native decorations
/// leave no custom title bar and each reporting its action, the frame-rate readout only while Render
/// Metrics is on, and the lower-priority parts left out at a narrow window rather than overlapping.

#include "App/StatusBarView.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(), the log buffer

#include <optional>
#include <string>

namespace App
{
namespace
{

using StatusBarView::Action;

constexpr const char* PROCESSES = ICON_FA_LIST "  412 processes";
constexpr const char* INTERVAL = ICON_FA_ARROWS_ROTATE "  Updates every 1 s";

class StatusBarViewRenderTest : public ::testing::Test
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
        m_Input = {.access = std::nullopt,
                   .reducedPrivileges = false,
                   .networkCountersBlocked = false,
                   .processText = PROCESSES,
                   .intervalText = INTERVAL,
                   .showControls = false,
                   .fps = std::nullopt,
                   .frameTimeSeconds = 0.0F};
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame, and the text it drew (ImGui's own logging, from a window standing in for the shell).
    std::string frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        // Behind the bar, as ShellLayer's main window is (NoBringToFrontOnFocus), so the bar can be hovered.
        ImGui::Begin(
            "Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::LogToBuffer();
        m_LastAction = StatusBarView::render(m_Input);
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    /// Presses the status bar's button @p label on the next frame.
    static void press(const char* label)
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##StatusBar");
        ASSERT_NE(bar, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, bar->ID));
    }

    StatusBarView::Input m_Input;
    Action m_LastAction = Action::None;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(StatusBarViewRenderTest, ShowsTheProcessCountAndTheInterval)
{
    const std::string text = frame();
    EXPECT_TRUE(text.contains("412 processes")) << text;
    EXPECT_TRUE(text.contains("Updates every 1 s")) << text;
    EXPECT_FALSE(text.contains("FPS")) << text; // Render Metrics is off
    EXPECT_EQ(m_LastAction, Action::None);
}

TEST_F(StatusBarViewRenderTest, TheIconsShowOnlyWhenTheyApply)
{
    EXPECT_FALSE(frame().contains(ICON_FA_LOCK));
    m_Input.reducedPrivileges = true;
    EXPECT_TRUE(frame().contains(ICON_FA_LOCK));
    m_Input.networkCountersBlocked = true;
    EXPECT_TRUE(frame().contains(ICON_FA_NETWORK_WIRED));
}

// #2728: where the platform has the access chip, it replaces the plain lock: Standard with a lock,
// which offers Administrator when clicked, or Administrator with a shield, which offers nothing.
TEST_F(StatusBarViewRenderTest, TheAccessChipSaysStandardOrAdministratorAndStandardOffersAdministrator)
{
    m_Input.reducedPrivileges = true;
    m_Input.access = StatusBarView::AccessChip::Standard;
    const std::string standard = frame();
    EXPECT_TRUE(standard.contains(ICON_FA_LOCK " Standard")) << standard;
    press(StatusBarView::STANDARD_CHIP_LABEL);
    static_cast<void>(frame());
    EXPECT_EQ(m_LastAction, Action::Access);

    m_Input.access = StatusBarView::AccessChip::Administrator;
    const std::string administrator = frame();
    EXPECT_TRUE(administrator.contains(ICON_FA_SHIELD_HALVED " Administrator")) << administrator;
    EXPECT_FALSE(administrator.contains(ICON_FA_LOCK)) << "the chip, not the lock as well";
    press(StatusBarView::ADMINISTRATOR_CHIP_LABEL);
    static_cast<void>(frame());
    EXPECT_EQ(m_LastAction, Action::None);
}

TEST_F(StatusBarViewRenderTest, TheFrameRateReadoutShowsWhileRenderMetricsIsOn)
{
    m_Input.fps = 60.0F;
    m_Input.frameTimeSeconds = 1.0F / 60.0F;
    const std::string text = frame();
    EXPECT_TRUE(text.contains("60.0 FPS (16.67 ms)")) << text;
}

TEST_F(StatusBarViewRenderTest, SettingsAndHelpAreThereOnlyWithoutTheCustomTitleBar)
{
    // With the custom title bar its own buttons are the ones; a press here finds nothing.
    static_cast<void>(frame());
    press(StatusBarView::SETTINGS_LABEL);
    static_cast<void>(frame());
    EXPECT_EQ(m_LastAction, Action::None);

    m_Input.showControls = true;
    static_cast<void>(frame());
    press(StatusBarView::SETTINGS_LABEL);
    static_cast<void>(frame());
    EXPECT_EQ(m_LastAction, Action::Settings);
    press(StatusBarView::HELP_LABEL);
    static_cast<void>(frame());
    EXPECT_EQ(m_LastAction, Action::Help);
}

TEST_F(StatusBarViewRenderTest, ANarrowWindowLeavesOutTheLowerPriorityParts)
{
    m_Input.fps = 60.0F;
    m_Input.frameTimeSeconds = 1.0F / 60.0F;
    ImGui::GetIO().DisplaySize = ImVec2(160.0F, 400.0F);
    const std::string text = frame();
    EXPECT_FALSE(text.contains("Updates every")) << text; // the interval goes before the count
    EXPECT_FALSE(text.contains("FPS")) << text;
}

// #1395: hovering along the bar shows what each icon and button means.
TEST_F(StatusBarViewRenderTest, EachIconAndButtonExplainsItselfOnHover)
{
    m_Input.reducedPrivileges = true;
    m_Input.networkCountersBlocked = true;
    m_Input.showControls = true;
    static_cast<void>(frame());
    ImGuiWindow* bar = ImGui::FindWindowByName("##StatusBar");
    ASSERT_NE(bar, nullptr);
    // Over the stand-in shell window, as it is over the app's main window.
    ImGui::BringWindowToDisplayFront(bar);
    const float y = bar->Pos.y + (bar->Size.y * 0.5F);

    std::string seen;
    const int steps = static_cast<int>(bar->Size.x / 4.0F);
    for (int step = 0; step < steps; ++step) // a 4 px sweep along the bar
    {
        ImGui::GetIO().AddMousePosEvent(bar->Pos.x + 2.0F + (4.0F * static_cast<float>(step)), y);
        static_cast<void>(frame());
        seen += frame();
    }
    EXPECT_TRUE(seen.contains("Limited: some details of other users' processes, or actions on them, are unavailable")) << seen;
    EXPECT_TRUE(seen.contains("Per-process network counters are blocked on this system")) << seen;
    EXPECT_TRUE(seen.contains("Settings (F2)")) << seen;
    EXPECT_TRUE(seen.contains("Help: keyboard shortcuts, columns and tabs (F1)")) << seen;
}

// #1395: at the very narrowest the process count itself is cut short with an ellipsis, not drawn past
// the bar.
TEST_F(StatusBarViewRenderTest, AtTheNarrowestTheProcessCountIsCutShort)
{
    ImGui::GetIO().DisplaySize = ImVec2(90.0F, 400.0F);
    const std::string text = frame();
    EXPECT_FALSE(text.contains("412 processes")) << text;
    EXPECT_TRUE(text.contains("\xE2\x80\xA6")) << text; // U+2026
}

} // namespace
} // namespace App
