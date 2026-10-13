/// @file test_ElevationNoticeDialogRender.cpp
/// @brief The Limited Data notice, headless (#1547): it opens fitted to its text between its floor and
/// its authored width (#1601), centred on the window and re-centred when the window is resized, stays
/// inside a small window, and OK closes it and reports the "Don't show again" choice -- which the
/// layer, not the dialog, saves.

#include "App/DialogGeometry.h"
#include "App/ElevationNoticeDialog.h"
#include "App/ElevationNoticeText.h"
#include "UI/DialogMetrics.h"
#include "UI/DpiScale.h"
#include "UI/StyleScale.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): the modal and its controls

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

/// The words on each line of @p text when ImGui wraps it at @p wrapWidthPx in @p font at @p sizePx,
/// paragraph by paragraph: ImGui's own wrap, so the test sees the lines the notice draws.
[[nodiscard]] std::vector<std::vector<std::size_t>>
wrappedLineWordCounts(ImFont* font, float sizePx, std::string_view text, float wrapWidthPx)
{
    std::vector<std::vector<std::size_t>> paragraphs;
    for (const auto paragraph : std::views::split(text, '\n'))
    {
        const std::string_view body(paragraph.begin(), paragraph.end());
        if (body.empty())
        {
            continue;
        }
        std::vector<std::size_t>& lines = paragraphs.emplace_back();
        const char* cursor = body.data();
        const char* const end = body.data() + body.size();
        while (cursor < end)
        {
            const char* lineEnd = font->CalcWordWrapPosition(sizePx, cursor, end, wrapWidthPx);
            const std::string_view line(cursor, lineEnd);
            lines.push_back(static_cast<std::size_t>(
                std::ranges::count_if(std::views::split(line, ' '), [](const auto word) { return !std::ranges::empty(word); })));
            cursor = lineEnd;
            while (cursor < end && *cursor == ' ')
            {
                ++cursor; // A wrap skips the blanks it broke at
            }
        }
    }
    return paragraphs;
}

/// A paragraph wrapped onto more than one line with a line holding just one word (#1986).
[[nodiscard]] bool hasALoneWordLine(const std::vector<std::vector<std::size_t>>& paragraphs)
{
    return std::ranges::any_of(paragraphs,
                               [](const std::vector<std::size_t>& lines)
                               { return lines.size() > 1 && std::ranges::any_of(lines, [](std::size_t words) { return words == 1; }); });
}

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

// Escape dismisses it as OK, its only button, does (#1944), keeping the box's choice.
TEST_F(ElevationNoticeDialogRenderTest, EscapeClosesItAsOkDoes)
{
    ASSERT_TRUE(openAndSettle().open);
    press("Don't show again");
    EXPECT_FALSE(runFrame().dismissed);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_TRUE(runFrame().dismissed);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    EXPECT_TRUE(m_State.dontShowAgain);
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

// #1986: the notice in the app's own body font, Inter, at the Small and Medium presets over the common
// display scales. At Small on a 175 % display the first sentence came out a whisker wider than the
// 45 em cap and "privileges." wrapped onto a line of its own. The notice's own lines are checked
// through ImGui's wrap at the width the dialog took; every platform's wording through computeWidth().
class ElevationNoticeFontTest : public ElevationNoticeDialogRenderTest
{
  protected:
    /// Inter at @p regularPt on a @p displayScale display, with the style sizes the notice reads scaled
    /// as UI::Theme scales them (UI::computeStyleScale()).
    void useInter(float regularPt, float displayScale)
    {
        const float sizePx = UI::computePointsToPixels(regularPt, displayScale);
        const float styleScale = UI::computeStyleScale(regularPt, displayScale);
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(8.0F * styleScale, 8.0F * styleScale);
        style.FramePadding = ImVec2(UI::Theme::FRAME_PADDING_X * styleScale, UI::Theme::FRAME_PADDING_Y * styleScale);
        style.ItemSpacing = ImVec2(8.0F * styleScale, 4.0F * styleScale);
        style.ItemInnerSpacing = ImVec2(4.0F * styleScale, 4.0F * styleScale);

        const std::filesystem::path inter = std::filesystem::path(TASKSMACK_SOURCE_ASSETS_DIR) / "fonts" / "Inter-Regular.ttf";
        ASSERT_TRUE(std::filesystem::exists(inter)) << inter.string();
        ImGuiIO& io = ImGui::GetIO();
        if (m_Inter == nullptr)
        {
            m_Inter = io.Fonts->AddFontFromFileTTF(inter.string().c_str(), sizePx);
            ASSERT_NE(m_Inter, nullptr);
            io.FontDefault = m_Inter;
        }
        ImGui::GetStyle()._NextFrameFontSizeBase = sizePx; // NewFrame() applies the size from here
    }

    static constexpr std::array SCALES{1.0F, 1.25F, 1.5F, 1.75F, 2.0F};
    static constexpr std::array PRESETS{UI::FontSize::Small, UI::FontSize::Medium};

    ImFont* m_Inter = nullptr;
};

TEST_F(ElevationNoticeFontTest, NoLineIsALoneWordAtSmallOrMedium)
{
    ImGui::GetIO().DisplaySize = ImVec2(1200.0F, 800.0F); // The window #1986 was found in
    m_State.openRequested = true;
    for (const UI::FontSize preset : PRESETS)
    {
        for (const float scale : SCALES)
        {
            const auto& config = UI::FONT_SIZE_PRESETS.at(static_cast<std::size_t>(preset));
            const float sizePx = UI::computePointsToPixels(config.regularPt, scale);
            SCOPED_TRACE(std::string(config.name) + " at " + std::to_string(scale));
            useInter(config.regularPt, scale);
            const Measured measured = settle();
            ASSERT_TRUE(measured.open);
            ASSERT_NEAR(measured.emPx, sizePx, 1.0F); // ImGui rounds the size to a whole pixel
            const ImGuiWindow* modal = ImGui::FindWindowByName(ElevationNoticeDialog::POPUP_ID);
            ASSERT_NE(modal, nullptr);
            const float wrapWidth = modal->WorkRect.GetWidth();
            const auto lines = wrappedLineWordCounts(m_Inter, measured.emPx, ElevationNoticeText::forCurrentPlatform(), wrapWidth);
            EXPECT_FALSE(hasALoneWordLine(lines)) << "wrap width " << wrapWidth;
            expectInsideDisplay(measured);
        }
    }
}

TEST_F(ElevationNoticeFontTest, NoPlatformsWordingLeavesALoneWordAtSmallOrMedium)
{
    constexpr float VIEWPORT_WIDTH = 1200.0F;
    for (const std::string_view text : {ElevationNoticeText::LINUX, ElevationNoticeText::WINDOWS, ElevationNoticeText::OTHER})
    {
        for (const UI::FontSize preset : PRESETS)
        {
            for (const float scale : SCALES)
            {
                const auto& config = UI::FONT_SIZE_PRESETS.at(static_cast<std::size_t>(preset));
                SCOPED_TRACE(std::string(config.name) + " at " + std::to_string(scale) + ": " + std::string(text.substr(0, 24)));
                useInter(config.regularPt, scale);
                ImGui::NewFrame();
                // ImGui truncates the window's width to a whole pixel; the body wraps inside the padding.
                const float wrapWidth =
                    std::floor(ElevationNoticeDialog::computeWidth(text, VIEWPORT_WIDTH)) - (ImGui::GetStyle().WindowPadding.x * 2.0F);
                const float emPx = ImGui::GetFontSize();
                ImGui::EndFrame();
                EXPECT_FALSE(hasALoneWordLine(wrappedLineWordCounts(m_Inter, emPx, text, wrapWidth))) << "wrap width " << wrapWidth;
            }
        }
    }
}

} // namespace
} // namespace App
