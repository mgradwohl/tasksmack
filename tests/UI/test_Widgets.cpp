#include "UI/Widgets.h"

#include <gtest/gtest.h>
#include <imgui.h>

namespace UI::Widgets
{
namespace
{

class ImGuiFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        IMGUI_CHECKVERSION();
        m_Context = ImGui::CreateContext();
        ASSERT_NE(m_Context, nullptr);
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
        m_Context = nullptr;
    }

    void beginFrame()
    {
        ImGui::NewFrame();
        ImGui::Begin("WidgetsTestWindow");
    }

    void endFrame()
    {
        ImGui::End();
        ImGui::Render();
    }

    ImGuiContext* m_Context = nullptr;
};

TEST(WidgetsHeaderTest, MinBarFillHeightConstantIsOnePixel)
{
    EXPECT_FLOAT_EQ(MIN_BAR_FILL_HEIGHT, 1.0F);
}

TEST_F(ImGuiFixture, DrawRightAlignedOverlayTextHandlesNullAndEmpty)
{
    beginFrame();
    ImGui::Button("BaseItem");

    EXPECT_NO_THROW(drawRightAlignedOverlayText(nullptr));
    EXPECT_NO_THROW(drawRightAlignedOverlayText(""));

    endFrame();
}

TEST_F(ImGuiFixture, DrawRightAlignedOverlayTextDrawsNonEmptyText)
{
    beginFrame();
    ImGui::Button("BaseItem");
    EXPECT_NO_THROW(drawRightAlignedOverlayText("Overlay", 6.0F));
    endFrame();
}

TEST_F(ImGuiFixture, DrawVerticalBarWithValueClampsInputAndRenders)
{
    beginFrame();
    const ImVec4 barColor{0.2F, 0.6F, 0.9F, 1.0F};

    EXPECT_NO_THROW(drawVerticalBarWithValue("bar-neg", -0.5F, barColor, 100.0F, 24.0F, "0%", "CPU", "Tooltip"));
    EXPECT_NO_THROW(drawVerticalBarWithValue("bar-over", 1.5F, barColor, 100.0F, 24.0F, "100%", "CPU", "Tooltip"));
    EXPECT_NO_THROW(drawVerticalBarWithValue("bar-dbl", 0.25, barColor, 100.0F, 24.0F, "25%", "CPU", nullptr));

    endFrame();
}

} // namespace
} // namespace UI::Widgets
