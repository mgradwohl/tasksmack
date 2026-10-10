/// @file test_AboutDialogRender.cpp
/// @brief The About dialog, headless (#1490): it opens at its authored width and stays there -- it
/// used to auto-fit around a wrapped shortcut table and creep wider every frame until it filled most
/// of the window -- its height is held to its own cap with the contents scrolling inside it, it
/// stays inside a small window with its OK button reachable, and OK or Escape closes it. Since #172
/// it has no shortcuts section; those are in the Help window (test_HelpWindowRender.cpp). Since #1600
/// it has the Help window's user guide and "Report a problem" links, and AboutLayer opens it on the
/// Core::OpenAboutEvent the title bar's "i" raises.

#include "App/AboutDialog.h"
#include "App/AboutLayer.h"
#include "App/DialogGeometry.h"
#include "App/HelpContent.h"
#include "Core/ApplicationEvents.h"
#include "UI/DialogMetrics.h"
#include "UI/IconLoader.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal, its body and its OK button

#include <algorithm>
#include <string>
#include <string_view>

namespace App
{
namespace
{

/// The open modal's geometry, and its scrolling body's, at the end of a frame.
struct Measured
{
    bool open = false;
    bool visible = false; ///< The modal was drawn this frame (ImGui hides a new popup's first frame)
    float emPx = 0.0F;    ///< ImGui::GetFontSize() during the frame
    ImVec2 pos;
    ImVec2 size;
    float scrollMaxY = -1.0F;
    float bodyHeight = 0.0F;
    float bodyScrollMaxX = -1.0F;
    float bodyScrollMaxY = -1.0F;
    float scrollMaxX = -1.0F;
    float bodyContentWidth = 0.0F; ///< Width of the body's content region (inside any scrollbar)
    float bodyContentRight = 0.0F; ///< Screen x of that region's right edge
    float bodyContentUsed = 0.0F;  ///< Width the body's items reached (its ContentSize.x)
    float widestOverflowPx = 0.0F; ///< Most any table cell's content ran past its column's right edge
    std::string activatedUrl;      ///< What render() returned: the link pressed this frame, if any
};

class AboutDialogRenderTest : public ::testing::Test
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

    /// One frame of a window standing in for the main window, drawing the dialog as AboutLayer does.
    Measured runFrame()
    {
        Measured measured;
        ImGui::NewFrame();
        measured.emPx = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        measured.activatedUrl = AboutDialog::render(m_OpenRequested, m_Icon);
        measured.open = ImGui::IsPopupOpen(AboutDialog::POPUP_ID);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            measured.pos = modal->Pos;
            measured.size = modal->Size;
            measured.scrollMaxY = modal->ScrollMax.y;
            measured.scrollMaxX = modal->ScrollMax.x;
            measured.visible = !modal->Hidden;
            for (const ImGuiWindow* window : GImGui->Windows)
            {
                if (window->ParentWindow == modal && std::string_view{window->Name}.contains("##AboutBody"))
                {
                    measured.bodyHeight = window->Size.y;
                    measured.bodyScrollMaxY = window->ScrollMax.y;
                    measured.bodyScrollMaxX = window->ScrollMax.x;
                    measured.bodyContentWidth = window->ContentRegionRect.GetWidth();
                    measured.bodyContentRight = window->ContentRegionRect.Max.x;
                    measured.bodyContentUsed = window->ContentSize.x;
                    measured.widestOverflowPx = widestTableOverflow(window);
                }
            }
        }
        ImGui::Render();
        return measured;
    }

    /// Most any cell's content in a table drawn in @p window ran past the right edge of its column
    /// this frame -- what a table clips without a scrollbar, so a positive value is text cut off.
    static float widestTableOverflow(const ImGuiWindow* window)
    {
        float widest = 0.0F;
        for (int n = 0; n < GImGui->Tables.GetMapSize(); ++n)
        {
            const ImGuiTable* table = GImGui->Tables.TryGetMapData(n);
            if (table == nullptr || table->OuterWindow != window)
            {
                continue;
            }
            for (int column = 0; column < table->ColumnsCount; ++column)
            {
                const ImGuiTableColumn& col = table->Columns[column];
                widest = std::max(widest, col.ContentMaxXUnfrozen - col.MaxX);
            }
        }
        return widest;
    }

    /// Opens the dialog and runs enough frames for its height to settle.
    Measured openAndSettle()
    {
        m_OpenRequested = true;
        Measured measured;
        for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
        {
            measured = runFrame();
        }
        return measured;
    }

    /// Presses the dialog's OK button on the next frame.
    static void pressOk()
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr("OK", 0, modal->ID));
    }

    /// The dialog's scrolling body, or nullptr when it was not drawn.
    [[nodiscard]] static const ImGuiWindow* aboutBody()
    {
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (std::string_view{window->Name}.contains("##AboutBody"))
            {
                return window;
            }
        }
        return nullptr;
    }

    /// Presses, on the next frame, the link to @p url drawn directly in the body (not in a table):
    /// renderLink() pushes the URL as its ID, then draws its "##Link" button.
    static void pressLink(const char* url)
    {
        const ImGuiWindow* body = aboutBody();
        ASSERT_NE(body, nullptr);
        ImGui::ActivateItemByID(ImHashStr("##Link", 0, ImHashStr(url, 0, body->ID)));
    }

    [[nodiscard]] static float expectedWidth(const Measured& measured)
    {
        return UI::DialogMetrics::computeDialogWidth(measured.emPx, ABOUT_WIDTH_EM, ImGui::GetIO().DisplaySize.x);
    }

    [[nodiscard]] static float heightCap()
    {
        return UI::DialogMetrics::computeCompactDialogMaxExtent(ImGui::GetIO().DisplaySize.y, ABOUT_MAX_HEIGHT_FRACTION);
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

    bool m_OpenRequested = false;
    UI::Texture m_Icon; // Invalid: no GL context here, so the header keeps the icon's space empty

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(AboutDialogRenderTest, OpensAtItsAuthoredWidthAndNeverGrows)
{
    // The issue: the dialog crept wider frame after frame. Its width is now set every frame, so the
    // first frame it is seen on already has it, and the hundredth agrees.
    m_OpenRequested = true;
    Measured first = runFrame();
    int frames = 1;
    while (!first.visible && frames < SETTLE_FRAMES)
    {
        first = runFrame(); // ImGui hides a popup's first frame while it measures it
        ++frames;
    }
    ASSERT_TRUE(first.open);
    ASSERT_TRUE(first.visible);
    EXPECT_LE(frames, 2);
    EXPECT_FALSE(m_OpenRequested);
    EXPECT_FLOAT_EQ(first.size.x, expectedWidth(first));

    Measured later = first;
    for (int frame = 0; frame < 120; ++frame)
    {
        later = runFrame();
    }
    EXPECT_FLOAT_EQ(later.size.x, first.size.x);
}

TEST_F(AboutDialogRenderTest, IsCentredOnceItsHeightHasSettled)
{
    // Centred only when it appeared, on its first and shorter frame, it ended up hanging off the
    // bottom of the window once its body had fitted itself to its contents.
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    EXPECT_NEAR(measured.pos.x + (measured.size.x * 0.5F), display.x * 0.5F, 1.0F);
    EXPECT_NEAR(measured.pos.y + (measured.size.y * 0.5F), display.y * 0.5F, 1.0F);
}

TEST_F(AboutDialogRenderTest, IsCompactAtANormalWindow)
{
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    // Well under half the window wide, and no taller than its own cap -- not the 90 % every dialog
    // may reach.
    EXPECT_LT(measured.size.x, ImGui::GetIO().DisplaySize.x * 0.5F);
    EXPECT_LE(measured.size.y, heightCap());
    EXPECT_GT(measured.bodyHeight, 0.0F);
    EXPECT_FLOAT_EQ(measured.scrollMaxY, 0.0F);
    expectInsideDisplay(measured);
}

TEST_F(AboutDialogRenderTest, AtALargeFontTheBodyScrollsAndTheDialogStaysCapped)
{
    ImGui::GetStyle().FontScaleMain = 2.5F;
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    EXPECT_GT(measured.emPx, ImGui::GetIO().Fonts->Fonts[0]->LegacySize * 2.0F); // The scale took effect
    EXPECT_FLOAT_EQ(measured.size.x, expectedWidth(measured));
    EXPECT_LE(measured.size.y, heightCap() + 1.0F); // Rounded up to whole pixels
    // The contents scroll inside the body; the dialog itself never needs scrolling, so the OK row
    // below the body is always in view.
    EXPECT_GT(measured.bodyScrollMaxY, 0.0F);
    EXPECT_FLOAT_EQ(measured.scrollMaxY, 0.0F);
    expectInsideDisplay(measured);
}

TEST_F(AboutDialogRenderTest, StaysInsideASmallWindowWithItsButtonReachable)
{
    ImGui::GetIO().DisplaySize = ImVec2(640.0F, 400.0F);
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    EXPECT_LE(measured.size.x, UI::DialogMetrics::computeDialogMaxExtent(640.0F));
    EXPECT_LE(measured.size.y, heightCap() + 1.0F);
    EXPECT_GT(measured.bodyScrollMaxY, 0.0F);
    EXPECT_FLOAT_EQ(measured.scrollMaxY, 0.0F);
    expectInsideDisplay(measured);

    // The window shrinks further while the dialog is open: it follows.
    ImGui::GetIO().DisplaySize = ImVec2(480.0F, 320.0F);
    Measured shrunk = measured;
    for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
    {
        shrunk = runFrame();
    }
    EXPECT_FLOAT_EQ(shrunk.scrollMaxY, 0.0F);
    expectInsideDisplay(shrunk);
}

TEST_F(AboutDialogRenderTest, AtALargeFontInASmallWindowNothingIsClipped)
{
    // The body scrolls only vertically, so anything wider than it is cut off: at 2.5x in a 480px
    // window the header stacks the icon above the text, and the links and the version wrap
    // (#1490 review).
    ImGui::GetStyle().FontScaleMain = 2.5F;
    ImGui::GetIO().DisplaySize = ImVec2(480.0F, 400.0F);
    const Measured measured = openAndSettle();
    ASSERT_TRUE(measured.open);
    ASSERT_GT(measured.bodyContentWidth, 0.0F);
    EXPECT_LE(measured.bodyContentUsed, measured.bodyContentWidth + 0.5F);
    EXPECT_LE(measured.widestOverflowPx, 0.5F);
    EXPECT_FLOAT_EQ(measured.bodyScrollMaxX, 0.0F);
    EXPECT_FLOAT_EQ(measured.scrollMaxX, 0.0F);
    EXPECT_FLOAT_EQ(measured.scrollMaxY, 0.0F);
    expectInsideDisplay(measured);
}

TEST_F(AboutDialogRenderTest, OkClosesIt)
{
    ASSERT_TRUE(openAndSettle().open);
    pressOk();
    (void) runFrame(); // OK is pressed this frame
    EXPECT_FALSE(runFrame().open);
}

TEST_F(AboutDialogRenderTest, EscapeClosesIt)
{
    ASSERT_TRUE(openAndSettle().open);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    (void) runFrame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    EXPECT_FALSE(runFrame().open);
}

TEST_F(AboutDialogRenderTest, HasNoShortcutsSection)
{
    // #172: the shortcut table moved to the Help window; About is product information only.
    ASSERT_TRUE(openAndSettle().open);
    const ImGuiWindow* body = nullptr;
    for (const ImGuiWindow* window : GImGui->Windows)
    {
        if (std::string_view{window->Name}.contains("##AboutBody"))
        {
            body = window;
        }
    }
    ASSERT_NE(body, nullptr);
    EXPECT_NE(ImGui::TableFindByID(ImHashStr("##AboutProject", 0, body->ID)), nullptr); // The probe finds About's tables
    EXPECT_EQ(ImGui::TableFindByID(ImHashStr("##Shortcuts", 0, body->ID)), nullptr);
}

TEST_F(AboutDialogRenderTest, HasTheHelpWindowsUserGuideAndReportAProblemLinks)
{
    // #1600: About shows the same two links as the Help window, with the same URLs. Pressing one
    // returns its URL for AboutLayer to open; a link that was not drawn could not be pressed.
    ASSERT_TRUE(openAndSettle().open);
    for (const char* url : {HelpContent::USER_GUIDE_URL, HelpContent::ISSUES_URL})
    {
        SCOPED_TRACE(url);
        pressLink(url);
        std::string activated;
        int frame = 0;
        while (frame < 2 && activated.empty())
        {
            activated = runFrame().activatedUrl;
            ++frame;
        }
        EXPECT_EQ(activated, url);
        EXPECT_TRUE(runFrame().activatedUrl.empty()); // Once per press
    }
    EXPECT_TRUE(runFrame().open); // A link leaves the dialog open
}

TEST_F(AboutDialogRenderTest, NothingIsActivatedWithoutAPress)
{
    m_OpenRequested = true;
    for (int frame = 0; frame < SETTLE_FRAMES; ++frame)
    {
        EXPECT_TRUE(runFrame().activatedUrl.empty());
    }
}

TEST_F(AboutDialogRenderTest, TheTitleBarsOpenAboutEventOpensIt)
{
    // The title bar's "i" (#1600), like Help's link and Settings, raises Core::OpenAboutEvent;
    // AboutLayer opens the dialog on it, without consuming it.
    AboutLayer layer;
    const auto renderLayerFrame = [&layer]
    {
        ImGui::NewFrame();
        layer.onRender();
        const bool open = ImGui::IsPopupOpen(AboutDialog::POPUP_ID);
        ImGui::Render();
        return open;
    };
    EXPECT_FALSE(renderLayerFrame());

    Core::OpenAboutEvent event;
    layer.onEvent(event);
    EXPECT_FALSE(event.isHandled());
    EXPECT_TRUE(renderLayerFrame());
}

TEST_F(AboutDialogRenderTest, DrawsNothingUntilRequested)
{
    EXPECT_FALSE(runFrame().open);
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
}

} // namespace
} // namespace App
