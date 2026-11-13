/// @file test_AccessNoticeDialogRender.cpp
/// @brief The Linux Standard note (#2728, #3580), headless: it says how to start an Administrator session and
/// offers nothing that elevates this one; OK and Escape close it; it is centred and stays inside a small window.
/// On Windows it offers Restart as administrator..., says what the UAC prompt asks for, and how the last restart went.

#include "App/AccessChoice.h"
#include "App/AccessNoticeDialog.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its controls

#include <string>

namespace App
{
namespace
{

using AccessChoice::AccessPlatform;

struct Frame
{
    bool closed = false; ///< render() said OK or Escape closed it
    AccessNoticeDialog::Action action = AccessNoticeDialog::Action::None;
    bool open = false;
    std::string text; ///< What the notice drew
    ImVec2 pos;
    ImVec2 size;
};

class AccessNoticeDialogRenderTest : public ::testing::Test
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
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the font atlas
        io.Fonts->AddFontDefault();
        m_State.platform = AccessPlatform::Linux;
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window standing in for the main window, drawing the notice as the layer does.
    Frame runFrame()
    {
        Frame frame;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        ImGui::LogToBuffer();
        frame.action = AccessNoticeDialog::render(m_State);
        frame.closed = frame.action == AccessNoticeDialog::Action::Closed;
        frame.text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        frame.open = ImGui::IsPopupOpen(AccessNoticeDialog::POPUP_ID);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            frame.pos = modal->Pos;
            frame.size = modal->Size;
        }
        ImGui::Render();
        return frame;
    }

    /// Frames until the note has settled; whether any of them reported it closed.
    Frame settle()
    {
        Frame last;
        bool closed = false;
        bool restart = false;
        for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
        {
            last = runFrame();
            closed = closed || last.closed;
            restart = restart || last.action == AccessNoticeDialog::Action::RestartAsAdministrator;
        }
        last.closed = closed;
        if (restart)
        {
            last.action = AccessNoticeDialog::Action::RestartAsAdministrator;
        }
        return last;
    }

    Frame open()
    {
        AccessNoticeDialog::requestOpen(m_State);
        return settle();
    }

    /// Presses the footer button @p label on the next frame.
    static void press(const char* label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, modal->ID));
    }

    static constexpr int SETTLE_FRAMES = 6;
    AccessNoticeDialog::State m_State;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(AccessNoticeDialogRenderTest, ItSaysHowToStartAnAdministratorSessionAndOffersNothingThatElevatesThisOne)
{
    const Frame opened = open();
    ASSERT_TRUE(opened.open);
    EXPECT_TRUE(opened.text.contains("Some data is limited"));
    EXPECT_TRUE(opened.text.contains("(Administrator)")) << "how to start one (#3580)";
    EXPECT_TRUE(opened.text.contains("sudo"));
    EXPECT_FALSE(opened.text.contains("Authenticate")) << "no button switches this session";
    EXPECT_FALSE(opened.text.contains("password")) << "nothing asks for one after TaskSmack starts";
}

TEST_F(AccessNoticeDialogRenderTest, OkClosesIt)
{
    ASSERT_TRUE(open().open);
    press(AccessNoticeDialog::OK_LABEL);
    const Frame closed = settle();
    EXPECT_TRUE(closed.closed);
    EXPECT_FALSE(closed.open);
}

TEST_F(AccessNoticeDialogRenderTest, EscapeClosesIt)
{
    ASSERT_TRUE(open().open);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_TRUE(runFrame().closed);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    EXPECT_FALSE(settle().open);
}

TEST_F(AccessNoticeDialogRenderTest, TheTableShowsWhatAdministratorAddsAndWhatItNeverDoes)
{
    ASSERT_TRUE(open().open);
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr(AccessNoticeDialog::TABLE_HEADER, 0, modal->ID));
    const Frame expanded = settle();
    EXPECT_TRUE(expanded.text.contains("Other users' crash reports"));
    EXPECT_TRUE(expanded.text.contains("Never, even as Administrator"));
    EXPECT_TRUE(expanded.text.contains("Processes hidden by /proc's hidepid")) << "root lists them (#3580)";
    EXPECT_FALSE(expanded.text.contains("Authenticate")) << "a Standard session is never elevated after it starts";
    EXPECT_FALSE(expanded.text.contains("helper"));
}

TEST_F(AccessNoticeDialogRenderTest, ItIsCentredAndStaysInsideASmallWindow)
{
    Frame opened = open();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    EXPECT_NEAR(opened.pos.x + (opened.size.x * 0.5F), display.x * 0.5F, 1.0F);
    EXPECT_NEAR(opened.pos.y + (opened.size.y * 0.5F), display.y * 0.5F, 1.0F);

    ImGui::GetIO().DisplaySize = ImVec2(480.0F, 360.0F);
    opened = settle();
    EXPECT_GE(opened.pos.x, 0.0F);
    EXPECT_GE(opened.pos.y, 0.0F);
    EXPECT_LE(opened.pos.x + opened.size.x, 480.0F);
    EXPECT_LE(opened.pos.y + opened.size.y, 360.0F);
}

/// The Windows notice (#2728): Restart as administrator... beside the default Continue as Standard.
class WindowsAccessNoticeDialogRenderTest : public AccessNoticeDialogRenderTest
{
  protected:
    void SetUp() override
    {
        AccessNoticeDialogRenderTest::SetUp();
        m_State.platform = AccessPlatform::Windows;
        m_State.restart = AccessNoticeDialog::Restart::Ready;
    }
};

TEST_F(WindowsAccessNoticeDialogRenderTest, ItOffersRestartAsAdministratorAndSaysWhatWindowsAsks)
{
    const Frame opened = open();
    ASSERT_TRUE(opened.open);
    EXPECT_TRUE(opened.text.contains(AccessNoticeDialog::CONTINUE_LABEL));
    EXPECT_TRUE(opened.text.contains("Restart as administrator..."));
    EXPECT_TRUE(opened.text.contains("restarts TaskSmack elevated"));
    EXPECT_TRUE(opened.text.contains("Windows asks for approval (UAC)"));
    EXPECT_FALSE(opened.text.contains("sudo"));
    EXPECT_FALSE(opened.text.contains("password")) << "an administrator only approves";
}

TEST_F(WindowsAccessNoticeDialogRenderTest, AStandardUserIsToldAnAdministratorsPasswordIsNeeded)
{
    m_State.credentials = true;
    const Frame opened = open();
    EXPECT_TRUE(opened.text.contains("name and password")) << opened.text;
    EXPECT_TRUE(opened.text.contains("Restart as administrator...")) << "the option stays for a standard user";
}

TEST_F(WindowsAccessNoticeDialogRenderTest, RestartAsksForItAndTheNoticeStaysUp)
{
    ASSERT_TRUE(open().open);
    press(AccessNoticeDialog::RESTART_LABEL);
    const Frame pressed = settle();
    EXPECT_EQ(pressed.action, AccessNoticeDialog::Action::RestartAsAdministrator);
    EXPECT_FALSE(pressed.closed);
    EXPECT_TRUE(pressed.open) << "it waits there for Windows' answer";
}

TEST_F(WindowsAccessNoticeDialogRenderTest, WhileWindowsAsksRestartCantBePressedAgain)
{
    ASSERT_TRUE(open().open);
    m_State.restart = AccessNoticeDialog::Restart::Waiting;
    press(AccessNoticeDialog::RESTART_LABEL);
    const Frame waiting = settle();
    EXPECT_EQ(waiting.action, AccessNoticeDialog::Action::None);
    EXPECT_TRUE(waiting.text.contains(AccessNoticeDialog::WAITING_NOTE));
}

TEST_F(WindowsAccessNoticeDialogRenderTest, ARunThatCantRestartSaysWhyAndOffersNothing)
{
    m_State.restart = AccessNoticeDialog::Restart::Unavailable;
    ASSERT_TRUE(open().open);
    press(AccessNoticeDialog::RESTART_LABEL);
    const Frame frame = settle();
    EXPECT_EQ(frame.action, AccessNoticeDialog::Action::None);
    EXPECT_TRUE(frame.text.contains("can't restart itself")) << frame.text;
}

TEST_F(WindowsAccessNoticeDialogRenderTest, ADeclinedPromptIsSaidAndOpeningAgainClearsIt)
{
    m_State.outcome = std::string(AccessNoticeDialog::DECLINED_NOTE);
    ASSERT_TRUE(settle().text.empty()) << "nothing drawn before it is asked for";
    AccessNoticeDialog::requestOpen(m_State);
    EXPECT_TRUE(m_State.outcome.empty()) << "a new open starts without the last answer";
    m_State.outcome = std::string(AccessNoticeDialog::DECLINED_NOTE);
    const Frame declined = settle();
    EXPECT_TRUE(declined.text.contains("didn't restart TaskSmack")) << declined.text;
}

TEST_F(WindowsAccessNoticeDialogRenderTest, ContinueAsStandardAndEscapeCloseIt)
{
    ASSERT_TRUE(open().open);
    press(AccessNoticeDialog::CONTINUE_LABEL);
    const Frame closed = settle();
    EXPECT_TRUE(closed.closed);
    EXPECT_FALSE(closed.open);

    ASSERT_TRUE(open().open);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_TRUE(runFrame().closed);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    EXPECT_FALSE(settle().open);
}

TEST(AccessNoticeDialogTest, AFailedRestartIsOneSentence)
{
    EXPECT_EQ(AccessNoticeDialog::restartFailedNote("Access is denied.\r\n"),
              "TaskSmack couldn't restart as administrator: Access is denied. It carries on as Standard.");
    EXPECT_EQ(AccessNoticeDialog::restartFailedNote("no process"),
              "TaskSmack couldn't restart as administrator: no process. It carries on as Standard.");
}

} // namespace
} // namespace App
