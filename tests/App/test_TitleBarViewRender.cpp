/// @file test_TitleBarViewRender.cpp
/// @brief The custom title bar, headless (#1547): its buttons sit where the layout says (so the drag
/// area leaves them out), each reports its action for TitleBarLayer to carry out, Maximize becomes
/// Restore on a maximized window, the icon is drawn and bounded only when there is one, the Linux
/// system menu reports its window commands, and the bar reports the width it needs.

#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "App/TitleBarView.h"
#include "UI/IconLoader.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons

namespace App
{
namespace
{

using TitleBarView::Action;

constexpr float WIDTH = 1280.0F;
constexpr float HEIGHT = 720.0F;
constexpr float BAR = 32.0F;

class TitleBarViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(WIDTH, HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the font atlas
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    TitleBarView::Output frame(bool maximized = false, bool openSystemMenu = false)
    {
        ImGui::NewFrame();
        const TitleBarView::Output output = TitleBarView::render({.windowWidth = WIDTH,
                                                                  .windowHeight = HEIGHT,
                                                                  .barHeight = BAR,
                                                                  .maximized = maximized,
                                                                  .icon = &m_Icon,
                                                                  .openSystemMenu = openSystemMenu});
        ImGui::Render();
        return output;
    }

    /// Presses the bar's button @p label on the next frame.
    static void press(const char* label)
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar");
        ASSERT_NE(bar, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, bar->ID));
    }

    UI::Texture m_Icon; // Invalid: no GL context here, so no icon is drawn

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(TitleBarViewRenderTest, NothingIsAskedForUntilAButtonIsPressed)
{
    EXPECT_EQ(frame().action, Action::None);
    EXPECT_EQ(frame().action, Action::None);
}

TEST_F(TitleBarViewRenderTest, TheButtonsSitWhereTheLayoutSays)
{
    const TitleBarView::Output output = frame();
    // The same layout TitleBarLayer gives the hit test, from the bar's own geometry.
    const float buttonWidth = computeTitleBarButtonWidth(BAR, TITLE_BAR_BUTTON_ASPECT);
    const TitleBarButtonLayout expected = computeTitleBarButtonLayout(WIDTH, buttonWidth, BAR, BAR * TITLE_BAR_SEPARATOR_GAP_RATIO);
    EXPECT_FLOAT_EQ(output.layout.close.minX, expected.close.minX);
    EXPECT_FLOAT_EQ(output.layout.close.maxX, WIDTH); // Close is flush with the right edge
    EXPECT_FLOAT_EQ(output.layout.help.minX, expected.help.minX);
    EXPECT_LT(output.layout.about.maxX, output.layout.minimize.minX); // the app buttons stand apart
    EXPECT_GT(output.contentWidth, 0.0F);
    EXPECT_LT(output.contentWidth, WIDTH);
}

TEST_F(TitleBarViewRenderTest, EachButtonReportsItsAction)
{
    static_cast<void>(frame());
    // ImGui hashes a button's whole label, glyph included. Close carries the glyph in its label when
    // the chrome icon font isn't loaded, as here.
    press(ICON_FA_WINDOW_MINIMIZE "##Minimize");
    EXPECT_EQ(frame().action, Action::Minimize);
    press(ICON_FA_WINDOW_MAXIMIZE "##Maximize");
    EXPECT_EQ(frame().action, Action::Maximize);
    press(ICON_FA_XMARK "##Close");
    EXPECT_EQ(frame().action, Action::Close);
    press(TitleBarButtons::SETTINGS_LABEL);
    EXPECT_EQ(frame().action, Action::Settings);
    press(TitleBarButtons::HELP_LABEL);
    EXPECT_EQ(frame().action, Action::Help);
    press(TitleBarButtons::ABOUT_LABEL);
    EXPECT_EQ(frame().action, Action::About);
}

TEST_F(TitleBarViewRenderTest, AMaximizedWindowOffersRestore)
{
    static_cast<void>(frame(true));
    press(ICON_FA_WINDOW_RESTORE "##Restore");
    EXPECT_EQ(frame(true).action, Action::Restore);
}

TEST_F(TitleBarViewRenderTest, WithoutAnIconNoneIsDrawnOrBounded)
{
    const TitleBarView::Output output = frame();
    EXPECT_FALSE(output.iconDrawn);
    EXPECT_GT(TitleBarView::iconSize(BAR), 0.0F);
    EXPECT_LE(TitleBarView::iconSize(BAR), BAR);
}

TEST_F(TitleBarViewRenderTest, TheSystemMenuReportsItsWindowCommands)
{
    static_cast<void>(frame(false, true)); // Alt+Space
    // Opened by the bar's window: between frames there is no current window to resolve the ID in, so
    // the open-popup stack is checked instead.
    ASSERT_EQ(GImGui->OpenPopupStack.size(), 1);
    const ImGuiWindow* popup = GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
    ASSERT_NE(popup, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_WINDOW_MINIMIZE "  Minimize", 0, popup->ID));
    EXPECT_EQ(frame().action, Action::Minimize);
}

} // namespace
} // namespace App
