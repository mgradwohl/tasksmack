/// @file test_AccessNoticeDialogRender.cpp
/// @brief Standard or Administrator, the startup notice (#2728), headless: Continue as Standard and Escape
/// close it as Standard; Administrator asks the caller to start it and waits, disabled, for how it went;
/// a refused prompt leaves it open with a note; it is centred and stays inside a small window; and the
/// Administrator option is disabled, with its reason, where the decision says so.

#include "App/AccessChoice.h"
#include "App/AccessNoticeDialog.h"
#include "App/Panels/AuthorizeWording.h"

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
    AccessNoticeDialog::Choice choice = AccessNoticeDialog::Choice::None;
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
        frame.choice = AccessNoticeDialog::render(m_State);
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

    /// Frames until the notice has settled; the choice any of them reported, if one did.
    Frame settle()
    {
        Frame last;
        AccessNoticeDialog::Choice seen = AccessNoticeDialog::Choice::None;
        for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
        {
            last = runFrame();
            if (last.choice != AccessNoticeDialog::Choice::None)
            {
                seen = last.choice;
            }
        }
        last.choice = seen;
        return last;
    }

    Frame open(const AccessChoice::NoticeInputs& inputs)
    {
        m_State.platform = inputs.platform;
        AccessNoticeDialog::requestOpen(m_State, AccessChoice::decideNotice(inputs));
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

TEST_F(AccessNoticeDialogRenderTest, ContinueAsStandardClosesItAsStandard)
{
    const Frame opened = open({.platform = AccessPlatform::Linux, .helperInstalled = true});
    ASSERT_TRUE(opened.open);
    EXPECT_TRUE(opened.text.contains("running as Standard"));
    press(AccessNoticeDialog::CONTINUE_LABEL);
    const Frame closed = settle();
    EXPECT_EQ(closed.choice, AccessNoticeDialog::Choice::Standard);
    EXPECT_FALSE(closed.open);
}

TEST_F(AccessNoticeDialogRenderTest, EscapeIsContinueAsStandard)
{
    ASSERT_TRUE(open({.platform = AccessPlatform::Linux, .helperInstalled = true}).open);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_EQ(runFrame().choice, AccessNoticeDialog::Choice::Standard);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    EXPECT_FALSE(settle().open);
}

TEST_F(AccessNoticeDialogRenderTest, AdministratorWaitsForThePromptThenClosesWhenItSucceeds)
{
    ASSERT_TRUE(open({.platform = AccessPlatform::Linux, .helperInstalled = true}).open);
    press(AuthorizeWording::LINUX_WORDING.switchSession);
    const Frame asked = settle();
    EXPECT_EQ(asked.choice, AccessNoticeDialog::Choice::Administrator);
    EXPECT_TRUE(asked.open);
    EXPECT_TRUE(m_State.authenticating);
    EXPECT_TRUE(asked.text.contains(AccessNoticeDialog::WAITING_TEXT));

    // While the prompt is up, neither Continue nor Escape closes it.
    press(AccessNoticeDialog::CONTINUE_LABEL);
    EXPECT_TRUE(settle().open);

    AccessNoticeDialog::finishAdministrator(m_State, true, {});
    const Frame done = settle();
    EXPECT_FALSE(done.open);
    EXPECT_EQ(done.choice, AccessNoticeDialog::Choice::None) << "closed as Administrator, not as Standard";
}

TEST_F(AccessNoticeDialogRenderTest, ARefusedPromptLeavesItOpenAsStandardWithANote)
{
    ASSERT_TRUE(open({.platform = AccessPlatform::Linux, .helperInstalled = true}).open);
    press(AuthorizeWording::LINUX_WORDING.switchSession);
    (void) settle();
    AccessNoticeDialog::finishAdministrator(m_State, false, "the password prompt was dismissed");
    const Frame after = settle();
    EXPECT_TRUE(after.open);
    EXPECT_FALSE(m_State.authenticating);
    EXPECT_TRUE(after.text.contains("Still Standard: the password prompt was dismissed"));
}

TEST_F(AccessNoticeDialogRenderTest, WithoutTheHelperAdministratorIsDisabledAndSaysWhy)
{
    const Frame opened = open({.platform = AccessPlatform::Linux, .helperInstalled = false});
    ASSERT_TRUE(opened.open);
    EXPECT_TRUE(opened.text.contains(".deb or .rpm"));
    press(AuthorizeWording::LINUX_WORDING.switchSession);
    const Frame pressed = settle();
    EXPECT_EQ(pressed.choice, AccessNoticeDialog::Choice::None) << "a disabled button is never reported";
    EXPECT_FALSE(m_State.authenticating);
}

TEST_F(AccessNoticeDialogRenderTest, TheTableShowsWhatAdministratorAddsAndWhatItNeverDoes)
{
    ASSERT_TRUE(open({.platform = AccessPlatform::Linux, .helperInstalled = true}).open);
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr(AccessNoticeDialog::TABLE_HEADER, 0, modal->ID));
    const Frame expanded = settle();
    EXPECT_TRUE(expanded.text.contains("Other users' crash reports"));
    EXPECT_TRUE(expanded.text.contains("Never, even as Administrator"));
    EXPECT_FALSE(expanded.text.contains("Processes hidden by /proc's hidepid")) << "the helper doesn't list them yet";
}

TEST_F(AccessNoticeDialogRenderTest, ItIsCentredAndStaysInsideASmallWindow)
{
    Frame opened = open({.platform = AccessPlatform::Linux, .helperInstalled = true});
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

} // namespace
} // namespace App
