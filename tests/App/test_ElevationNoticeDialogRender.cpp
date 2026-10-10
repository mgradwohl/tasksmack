/// @file test_ElevationNoticeDialogRender.cpp
/// @brief The Limited Data notice, headless (#1547): it opens fitted to its text between its floor and
/// its authored width (#1601), centred on the window and re-centred when the window is resized, stays
/// inside a small window, and OK closes it and reports the "Don't show again" choice -- which the
/// layer, not the dialog, saves.

#include "App/DialogGeometry.h"
#include "App/ElevationNoticeDialog.h"
#include "UI/DialogMetrics.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its controls

namespace App
{
namespace
{

/// The open modal's geometry at the end of a frame, and what render() returned.
struct Measured
{
    bool open = false;
    bool visible = false; ///< Drawn this frame (ImGui hides a new popup's first frame)
    bool dismissed = false;
    float emPx = 0.0F;
    float contentWidth = 0.0F; ///< measureContentWidth() during the frame
    ImVec2 pos;
    ImVec2 size;
};

class ElevationNoticeDialogRenderTest : public ::testing::Test
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
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window standing in for the main window, drawing the notice as the layer does.
    Measured runFrame()
    {
        Measured measured;
        ImGui::NewFrame();
        measured.emPx = ImGui::GetFontSize();
        measured.contentWidth = ElevationNoticeDialog::measureContentWidth();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        measured.dismissed = ElevationNoticeDialog::render(m_State);
        measured.open = ImGui::IsPopupOpen(ElevationNoticeDialog::POPUP_ID);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            measured.pos = modal->Pos;
            measured.size = modal->Size;
            measured.visible = !modal->Hidden;
        }
        ImGui::Render();
        return measured;
    }

    Measured openAndSettle()
    {
        m_State.openRequested = true;
        return settle();
    }

    Measured settle()
    {
        Measured measured;
        for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
        {
            measured = runFrame();
        }
        return measured;
    }

    /// Activates the modal's item @p label on the next frame.
    static void press(const char* label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, modal->ID));
    }

    static void expectCentred(const Measured& measured)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        EXPECT_NEAR(measured.pos.x + (measured.size.x * 0.5F), display.x * 0.5F, 1.0F);
        EXPECT_NEAR(measured.pos.y + (measured.size.y * 0.5F), display.y * 0.5F, 1.0F);
    }

    static void expectInsideDisplay(const Measured& measured)
    {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        EXPECT_GE(measured.pos.x, 0.0F);
        EXPECT_GE(measured.pos.y, 0.0F);
        EXPECT_LE(measured.pos.x + measured.size.x, display.x);
        EXPECT_LE(measured.pos.y + measured.size.y, display.y);
    }

    static constexpr int SETTLE_FRAMES = 6;

    ElevationNoticeDialog::State m_State;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ElevationNoticeDialogRenderTest, StaysClosedUntilRequested)
{
    const Measured measured = settle();
    EXPECT_FALSE(measured.open);
    EXPECT_FALSE(measured.dismissed);
}

TEST_F(ElevationNoticeDialogRenderTest, OpensFittedToItsTextAndCentred)
{
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    ASSERT_TRUE(measured.visible);
    EXPECT_FALSE(m_State.openRequested);
    const float expected = UI::DialogMetrics::computeFittedDialogWidth(
        measured.contentWidth, measured.emPx, ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, ImGui::GetIO().DisplaySize.x);
    EXPECT_FLOAT_EQ(measured.size.x, expected);
    // Never wider than its authored width, never narrower than its floor.
    EXPECT_LE(measured.size.x, measured.emPx * ELEVATION_WIDTH_EM);
    EXPECT_GE(measured.size.x, measured.emPx * ELEVATION_MIN_WIDTH_EM);
    expectCentred(measured);
}

TEST_F(ElevationNoticeDialogRenderTest, HoldsItsWidthFrameAfterFrame)
{
    const Measured first = openAndSettle();
    ASSERT_TRUE(first.open);
    Measured later = first;
    for (int frame = 0; frame < 120; ++frame)
    {
        later = runFrame();
    }
    EXPECT_FLOAT_EQ(later.size.x, first.size.x);
    EXPECT_FLOAT_EQ(later.pos.x, first.pos.x);
}

TEST_F(ElevationNoticeDialogRenderTest, ReCentresWhenTheWindowIsResized)
{
    // #1601: centred only on the frame it appeared, a resize left it off to one side.
    ASSERT_TRUE(openAndSettle().open);
    ImGui::GetIO().DisplaySize = ImVec2(1920.0F, 1080.0F);
    const Measured larger = settle();
    ASSERT_TRUE(larger.open);
    expectCentred(larger);

    ImGui::GetIO().DisplaySize = ImVec2(1024.0F, 700.0F);
    const Measured smaller = settle();
    ASSERT_TRUE(smaller.open);
    expectCentred(smaller);
}

TEST_F(ElevationNoticeDialogRenderTest, ReCentresWhenTheFontGrows)
{
    const Measured before = openAndSettle();
    ASSERT_TRUE(before.open);
    ImGui::GetStyle().FontScaleMain = 1.5F;
    const Measured after = settle();
    ASSERT_TRUE(after.open);
    EXPECT_GT(after.size.x, before.size.x); // A font preset change widens it...
    expectCentred(after);                   // ...and it stays centred
}

TEST_F(ElevationNoticeDialogRenderTest, StaysInsideASmallWindow)
{
    ImGui::GetIO().DisplaySize = ImVec2(480.0F, 320.0F);
    ImGui::GetStyle().FontScaleMain = 2.0F;
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    EXPECT_LE(measured.size.x, UI::DialogMetrics::computeDialogMaxExtent(480.0F) + 1.0F);
    EXPECT_LE(measured.size.y, UI::DialogMetrics::computeDialogMaxExtent(320.0F) + 1.0F);
    expectInsideDisplay(measured);
}

TEST_F(ElevationNoticeDialogRenderTest, OkClosesItAndReportsTheChoice)
{
    ASSERT_TRUE(openAndSettle().open);
    press("OK");
    const Measured pressed = runFrame();
    EXPECT_TRUE(pressed.dismissed);
    EXPECT_FALSE(m_State.dontShowAgain); // Left unticked
    EXPECT_FALSE(settle().open);
}

TEST_F(ElevationNoticeDialogRenderTest, DontShowAgainIsReportedWithOk)
{
    ASSERT_TRUE(openAndSettle().open);
    press("Don't show again");
    EXPECT_FALSE(runFrame().dismissed); // Ticking it doesn't close the notice
    EXPECT_TRUE(m_State.dontShowAgain);
    press("OK");
    EXPECT_TRUE(runFrame().dismissed);
    EXPECT_TRUE(m_State.dontShowAgain);
}

TEST_F(ElevationNoticeDialogRenderTest, ReopensCentredAfterBeingDismissed)
{
    ASSERT_TRUE(openAndSettle().open);
    press("OK");
    (void) runFrame();
    ASSERT_FALSE(settle().open);
    ImGui::GetIO().DisplaySize = ImVec2(1280.0F, 720.0F);
    const Measured again = openAndSettle();
    ASSERT_TRUE(again.open);
    expectCentred(again);
}

} // namespace
} // namespace App
