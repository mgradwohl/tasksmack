/// @file test_HelpWindowRender.cpp
/// @brief The Help window, headless (#172): it opens as a resizable, non-modal window, lists every
/// SHORTCUT_HELP entry, its filter narrows the list, Escape closes it, it stays inside a small
/// window, and its "About TaskSmack..." link opens the About dialog -- drawn here as HelpLayer and
/// AboutLayer draw them.

#include "App/AboutDialog.h"
#include "App/HelpWindow.h"
#include "App/KeyboardShortcuts.h"
#include "UI/IconLoader.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the window, its filter and its link

#include <cstddef>
#include <string_view>

namespace App
{
namespace
{

class HelpWindowRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 900.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame: a window standing in for the main window, then Help and About as their layers draw
    /// them, Help's "About TaskSmack..." setting About's request as HelpLayer's event does.
    void runFrame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        if (HelpWindow::render(m_State) == HelpWindow::Action::OpenAbout)
        {
            m_AboutRequested = true;
        }
        AboutDialog::render(m_AboutRequested, m_Icon);
        m_AboutOpen = ImGui::IsPopupOpen(AboutDialog::POPUP_ID); // Inside the frame: it needs a current window
        ImGui::Render();
    }

    void runFrames(int count)
    {
        for (int frame = 0; frame < count; ++frame)
        {
            runFrame();
        }
    }

    void openAndSettle()
    {
        HelpWindow::requestOpen(m_State);
        runFrames(SETTLE_FRAMES);
    }

    /// The Help window, if it was drawn on the last frame.
    [[nodiscard]] static const ImGuiWindow* helpWindow()
    {
        const ImGuiWindow* window = ImGui::FindWindowByName(HelpWindow::WINDOW_ID);
        return (window != nullptr && window->Active) ? window : nullptr;
    }

    [[nodiscard]] static const ImGuiWindow* helpBody()
    {
        const ImGuiWindow* help = helpWindow();
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (help != nullptr && window->ParentWindow == help && std::string_view{window->Name}.contains(HelpWindow::BODY_ID))
            {
                return window;
            }
        }
        return nullptr;
    }

    void typeInFilter(const char* text)
    {
        const ImGuiWindow* body = helpBody();
        ASSERT_NE(body, nullptr);
        ImGui::ActivateItemByID(ImHashStr(HelpWindow::FILTER_ID, 0, body->ID));
        runFrames(2); // The field takes the keyboard
        ImGui::GetIO().AddInputCharactersUTF8(text);
        runFrames(2);
    }

    static constexpr int SETTLE_FRAMES = 6;

    HelpWindow::State m_State;
    bool m_AboutRequested = false;
    bool m_AboutOpen = false; ///< About's popup was open at the end of the last frame
    UI::Texture m_Icon;       // Invalid: no GL context here

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(HelpWindowRenderTest, DrawsNothingUntilRequested)
{
    runFrame();
    EXPECT_EQ(helpWindow(), nullptr);
}

TEST_F(HelpWindowRenderTest, OpensAsAResizableNonModalWindowInsideTheDisplay)
{
    openAndSettle();
    const ImGuiWindow* help = helpWindow();
    ASSERT_NE(help, nullptr);
    EXPECT_TRUE(m_State.open);
    EXPECT_EQ(help->Flags & ImGuiWindowFlags_NoResize, 0);
    EXPECT_EQ(help->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_Popup), 0);
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    EXPECT_GE(help->Pos.x, 0.0F);
    EXPECT_GE(help->Pos.y, 0.0F);
    EXPECT_LE(help->Pos.x + help->Size.x, display.x);
    EXPECT_LE(help->Pos.y + help->Size.y, display.y);
}

TEST_F(HelpWindowRenderTest, StaysInsideASmallWindow)
{
    ImGui::GetIO().DisplaySize = ImVec2(640.0F, 400.0F);
    openAndSettle();
    const ImGuiWindow* help = helpWindow();
    ASSERT_NE(help, nullptr);
    EXPECT_LE(help->Pos.x + help->Size.x, 640.0F);
    EXPECT_LE(help->Pos.y + help->Size.y, 400.0F);
    EXPECT_GE(help->Pos.x, 0.0F);
    EXPECT_GE(help->Pos.y, 0.0F);
}

TEST_F(HelpWindowRenderTest, ListsEveryShortcut)
{
    openAndSettle();
    EXPECT_EQ(m_State.shortcutRowsDrawn, KeyboardShortcuts::SHORTCUT_HELP.size());
}

TEST_F(HelpWindowRenderTest, TheFilterNarrowsTheList)
{
    openAndSettle();
    typeInFilter("quit");
    EXPECT_EQ(std::string_view{m_State.filter.data()}, "quit");
    EXPECT_EQ(m_State.visibleCount, 1U);
    EXPECT_EQ(m_State.shortcutRowsDrawn, 1U);
}

TEST_F(HelpWindowRenderTest, EscapeClosesIt)
{
    openAndSettle();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    runFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    runFrame();
    EXPECT_FALSE(m_State.open);
    EXPECT_EQ(helpWindow(), nullptr);
}

TEST_F(HelpWindowRenderTest, AboutLinkOpensAbout)
{
    openAndSettle();
    EXPECT_FALSE(m_AboutOpen);
    const ImGuiWindow* help = helpWindow();
    ASSERT_NE(help, nullptr);
    ImGui::ActivateItemByID(ImHashStr(HelpWindow::ABOUT_LINK_LABEL, 0, help->ID));
    runFrames(3);
    EXPECT_TRUE(m_AboutOpen);
    EXPECT_TRUE(m_State.open); // Help stays open behind About
}

TEST_F(HelpWindowRenderTest, RequestingItAgainKeepsItsFilter)
{
    openAndSettle();
    typeInFilter("ctrl");
    const std::size_t narrowed = m_State.visibleCount;
    ASSERT_LT(narrowed, KeyboardShortcuts::SHORTCUT_HELP.size());
    HelpWindow::requestOpen(m_State);
    runFrames(2);
    EXPECT_EQ(m_State.visibleCount, narrowed);
    EXPECT_EQ(m_State.shortcutRowsDrawn, narrowed);
}

} // namespace
} // namespace App
