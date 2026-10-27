/// @file test_TitleBarViewRender.cpp
/// @brief The custom title bar, headless (#1547): its buttons sit where the layout says (so the drag
/// area leaves them out), each reports its action for TitleBarLayer to carry out, Maximize becomes
/// Restore on a maximized window, the icon is drawn and bounded only when there is one, the Linux
/// system menu reports its window commands, and the bar reports the width it needs. Also the system
/// menu's item rule and the native Windows menu's command mapping, which need no ImGui (#3161).

#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "App/TitleBarView.h"
#include "Core/WindowGeometry.h"
#include "UI/IconLoader.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons

#include <string>
#include <string_view>

namespace App
{
namespace
{

using TitleBarView::Action;

constexpr float WIDTH = 1280.0F;
constexpr float HEIGHT = 720.0F;
constexpr float BAR = 32.0F;
constexpr float BORDER = 8.0F; // the resize border at a 1.0 display scale, as TitleBarLayer passes it

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
                                                                  .resizeBorderPx = BORDER,
                                                                  .icon = &m_Icon,
                                                                  .openSystemMenu = openSystemMenu});
        ImGui::Render();
        return output;
    }

    /// The ID of the bar's item @p label, as ImGui hashes it.
    [[nodiscard]] static ImGuiID itemId(const char* label)
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar");
        return (bar != nullptr) ? ImHashStr(label, 0, bar->ID) : 0;
    }

    /// Moves the mouse to (@p x, @p y) and runs two frames (ImGui's hover settles on the frame after the
    /// move), returning what the second one asked for.
    TitleBarView::Output moveMouseTo(float x, float y, bool maximized = false)
    {
        ImGui::GetIO().AddMousePosEvent(x, y);
        static_cast<void>(frame(maximized));
        return frame(maximized);
    }

    /// A left click where the mouse is: the press on one frame, the release on the next, returning what
    /// the release's frame asked for (ImGui::Button reports the click on the release).
    TitleBarView::Output click(bool maximized = false)
    {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(frame(maximized));
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        return frame(maximized);
    }

    /// Presses the bar's button @p label on the next frame.
    static void press(const char* label)
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar");
        ASSERT_NE(bar, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, bar->ID));
    }

    /// One frame, returning every string drawn in it (tooltips included).
    std::string frameText(bool maximized = false)
    {
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        static_cast<void>(TitleBarView::render({.windowWidth = WIDTH,
                                                .windowHeight = HEIGHT,
                                                .barHeight = BAR,
                                                .maximized = maximized,
                                                .icon = &m_Icon,
                                                .openSystemMenu = false}));
        std::string text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return text;
    }

    /// Rests the mouse on the middle of @p bounds long enough for a tooltip (ImGuiHoveredFlags_ForTooltip
    /// waits for a short stationary hover), then returns what the next frame draws.
    std::string hoverAndRead(const ButtonBounds& bounds)
    {
        ImGui::GetIO().AddMousePosEvent((bounds.minX + bounds.maxX) * 0.5F, BAR * 0.5F);
        for (int i = 0; i < HOVER_FRAMES; ++i)
        {
            static_cast<void>(frame());
        }
        return frameText();
    }

    /// The open system menu's window, opened as Alt+Space opens it.
    [[nodiscard]] const ImGuiWindow* openSystemMenu(bool maximized)
    {
        static_cast<void>(frame(maximized, true));
        return GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
    }

    static constexpr int HOVER_FRAMES = 60; // a second at 60 fps, well past ImGui's tooltip delay

    UI::Texture m_Icon; // Invalid: no GL context here, so no icon is drawn

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(TitleBarViewRenderTest, NothingIsAskedForUntilAButtonIsPressed)
{
    EXPECT_EQ(frame().action, Action::None);
    EXPECT_EQ(frame().action, Action::None);
}

// #2728: the bar draws the window's title, as the OS shows it, so an Administrator session says so here too.
TEST_F(TitleBarViewRenderTest, TheBarDrawsTheWindowsTitle)
{
    const auto drawnText = [](std::string_view title)
    {
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        static_cast<void>(TitleBarView::render({.windowWidth = WIDTH,
                                                .windowHeight = HEIGHT,
                                                .barHeight = BAR,
                                                .maximized = false,
                                                .resizeBorderPx = BORDER,
                                                .icon = nullptr,
                                                .openSystemMenu = false,
                                                .title = title}));
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return captured;
    };
    const std::string standard = drawnText("TaskSmack");
    EXPECT_TRUE(standard.contains("TaskSmack")) << standard;
    EXPECT_FALSE(standard.contains("Administrator")) << standard;
    const std::string administrator = drawnText("TaskSmack (Administrator)");
    EXPECT_TRUE(administrator.contains("TaskSmack (Administrator)")) << administrator;
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

// #2882: the Close button's slot reaches the window's top-right corner, which is the resize border's.
// TitleBarLayer keeps the press from the button; the bar must keep ImGui from hovering and pressing
// the button there too, or a corner resize would highlight the X and close the window on release.
TEST_F(TitleBarViewRenderTest, TheTopRightResizeCornerIsNotTheCloseButtons)
{
    static_cast<void>(frame());
    const ImGuiID corner = itemId(TitleBarView::TOP_RIGHT_RESIZE_CORNER_ID);
    const ImGuiID close = itemId(ICON_FA_XMARK "##Close"); // the glyph is in the label: no chrome icon font here
    ASSERT_NE(corner, 0U);
    ASSERT_NE(close, 0U);

    // The corner square, outermost and innermost pixel: the corner's item hovers, and a click closes nothing.
    static_cast<void>(moveMouseTo(WIDTH - 1.0F, 0.0F));
    EXPECT_EQ(GImGui->HoveredId, corner);
    EXPECT_EQ(click().action, Action::None);
    static_cast<void>(moveMouseTo(WIDTH - BORDER, BORDER - 1.0F));
    EXPECT_EQ(GImGui->HoveredId, corner);
    EXPECT_EQ(click().action, Action::None);

    // One pixel past the square, left of it or below it: the Close button, which a click closes with.
    static_cast<void>(moveMouseTo(WIDTH - BORDER - 1.0F, BORDER - 1.0F));
    EXPECT_EQ(GImGui->HoveredId, close);
    EXPECT_EQ(click().action, Action::Close);
    static_cast<void>(moveMouseTo(WIDTH - 1.0F, BORDER));
    EXPECT_EQ(GImGui->HoveredId, close);
    EXPECT_EQ(click().action, Action::Close);
}

TEST_F(TitleBarViewRenderTest, AMaximizedWindowsCloseButtonReachesTheCorner)
{
    // No resize borders on a maximized window: the corner is the button's, as on Windows.
    static_cast<void>(frame(true));
    static_cast<void>(moveMouseTo(WIDTH - 1.0F, 0.0F, true));
    EXPECT_EQ(GImGui->HoveredId, itemId(ICON_FA_XMARK "##Close"));
    EXPECT_EQ(click(true).action, Action::Close);
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

TEST_F(TitleBarViewRenderTest, TheSystemMenuOfAMaximizedWindowOffersRestoreNotMaximize)
{
    const ImGuiWindow* menu = openSystemMenu(true);
    ASSERT_NE(menu, nullptr);
    // Maximize is shown but disabled: pressing it does nothing, and the menu stays open.
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_WINDOW_MAXIMIZE "  Maximize", 0, menu->ID));
    EXPECT_EQ(frame(true).action, Action::None);
    ASSERT_FALSE(GImGui->OpenPopupStack.empty());
    // Restore is the way back.
    menu = GImGui->OpenPopupStack.back().Window;
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_WINDOW_RESTORE "  Restore", 0, menu->ID));
    EXPECT_EQ(frame(true).action, Action::Restore);
}

TEST_F(TitleBarViewRenderTest, TheSystemMenuOfANormalWindowOffersMaximizeAndClose)
{
    const ImGuiWindow* menu = openSystemMenu(false);
    ASSERT_NE(menu, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_WINDOW_MAXIMIZE "  Maximize", 0, menu->ID));
    EXPECT_EQ(frame().action, Action::Maximize);

    menu = openSystemMenu(false);
    ASSERT_NE(menu, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_XMARK "  Close", 0, menu->ID));
    EXPECT_EQ(frame().action, Action::Close);
}

// #3161: the item rule both menus follow -- the Linux one drawn above, and the native Windows one,
// which Windows would otherwise enable from an OS maximized state a client-side maximize never sets.
TEST(TitleBarSystemMenuTest, AMaximizedWindowsMenuOffersRestoreButNotMaximizeMoveOrSize)
{
    EXPECT_EQ(
        TitleBarView::systemMenuItems(true),
        (TitleBarView::SystemMenuItems{.restore = true, .move = false, .size = false, .minimize = true, .maximize = false, .close = true}));
}

TEST(TitleBarSystemMenuTest, ANormalWindowsMenuOffersMaximizeMoveAndSizeButNotRestore)
{
    EXPECT_EQ(
        TitleBarView::systemMenuItems(false),
        (TitleBarView::SystemMenuItems{.restore = false, .move = true, .size = true, .minimize = true, .maximize = true, .close = true}));
}

// #3161: what the native Windows menu chose is carried out as the title-bar buttons are, so Restore
// and Maximize are the client-side ones; Move and Size go to the OS.
TEST(TitleBarSystemMenuTest, TheNativeMenusWindowCommandsAreTheButtonsActions)
{
    using Core::WindowGeometry::decodeSystemCommand;
    using Core::WindowGeometry::SystemCommand;
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Restore), Action::Restore);
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Maximize), Action::Maximize);
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Minimize), Action::Minimize);
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Close), Action::Close);
    // As TrackPopupMenu() returns them: SC_RESTORE, SC_MAXIMIZE.
    EXPECT_EQ(TitleBarView::actionForSystemCommand(decodeSystemCommand(0xF120U)), Action::Restore);
    EXPECT_EQ(TitleBarView::actionForSystemCommand(decodeSystemCommand(0xF030U)), Action::Maximize);
}

TEST(TitleBarSystemMenuTest, TheNativeMenusMoveAndSizeAreLeftToTheOs)
{
    using Core::WindowGeometry::SystemCommand;
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Move), Action::None);
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Size), Action::None);
    EXPECT_EQ(TitleBarView::actionForSystemCommand(SystemCommand::Other), Action::None);
}

TEST_F(TitleBarViewRenderTest, TheAppButtonsSayWhatTheyDoOnHover)
{
    const TitleBarView::Output output = frame();
    EXPECT_TRUE(hoverAndRead(output.layout.settings).contains(TitleBarButtons::SETTINGS_TOOLTIP));
    EXPECT_TRUE(hoverAndRead(output.layout.help).contains(TitleBarButtons::HELP_TOOLTIP));
    EXPECT_TRUE(hoverAndRead(output.layout.about).contains(TitleBarButtons::ABOUT_TOOLTIP));
}

} // namespace
} // namespace App
