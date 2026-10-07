/// @file test_ChromeWidgets.cpp
/// @brief The shared section header and dialog footer (UI/ChromeWidgets.h, #1200), drawn in a
/// headless ImGui frame: where the footer's buttons actually land, and that a header is one item.

#include "UI/ChromeWidgets.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <functional>

namespace UI::Widgets
{
namespace
{

class ChromeWidgetsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a 600px-wide window running @p body.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, 400.0F));
        ImGui::Begin("Dialog", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    static constexpr float WINDOW_WIDTH = 600.0F;

  private:
    ImGuiContext* m_Context = nullptr;
};

struct Rect
{
    float minX = 0.0F;
    float maxX = 0.0F;
    float minY = 0.0F;
};

Rect lastItem()
{
    return {.minX = ImGui::GetItemRectMin().x, .maxX = ImGui::GetItemRectMax().x, .minY = ImGui::GetItemRectMin().y};
}

// [Cancel][Primary], the primary rightmost and ending at the content's right edge (#1200). The
// process-action confirm drew [Kill][Cancel] at the left.
TEST_F(ChromeWidgetsTest, FooterPutsThePrimaryActionRightmost)
{
    Rect primary;
    float contentRight = 0.0F;
    runFrame(
        [&]
        {
            contentRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const DialogFooterButton ok{.label = "Kill", .fills = nullptr, .tooltip = nullptr};
            const DialogFooterButton cancel{.label = "Cancel", .fills = nullptr, .tooltip = nullptr};
            (void) dialogFooter(ok, cancel, 100.0F);
            primary = lastItem(); // the primary is drawn last
        });
    EXPECT_FLOAT_EQ(primary.maxX, contentRight);
    EXPECT_FLOAT_EQ(primary.maxX - primary.minX, 100.0F);
}

TEST_F(ChromeWidgetsTest, LoneOkIsRightAlignedNotCentred)
{
    Rect ok;
    float contentRight = 0.0F;
    runFrame(
        [&]
        {
            contentRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            (void) dialogFooter({.label = "OK", .fills = nullptr, .tooltip = nullptr}, {}, 120.0F);
            ok = lastItem();
        });
    EXPECT_FLOAT_EQ(ok.maxX, contentRight);
}

// The header is one item, so a chart's value strip can share its line with SameLine(), and it
// covers the quieter detail as well as the title.
TEST_F(ChromeWidgetsTest, HeaderIsOneItemCoveringItsDetail)
{
    Rect titleOnly;
    Rect withDetail;
    bool hovered = true;
    float detailWidth = 0.0F;
    runFrame(
        [&]
        {
            hovered = sectionHeader(nullptr, "Network Throughput");
            titleOnly = lastItem();
            (void) sectionHeader(nullptr, "Network Throughput", "Ethernet 2", 300);
            withDetail = lastItem();
            detailWidth = ImGui::CalcTextSize("Ethernet 2").x;
        });
    EXPECT_FALSE(hovered);
    EXPECT_FLOAT_EQ(withDetail.minX, titleOnly.minX);
    EXPECT_GE(withDetail.maxX, titleOnly.maxX + detailWidth);
}

} // namespace
} // namespace UI::Widgets
