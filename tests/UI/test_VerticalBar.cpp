/// @file test_VerticalBar.cpp
/// @brief UI::Widgets::drawVerticalBarWithValue (UI/Widgets.h) in a headless ImGui frame (#1891): the
/// value and label it writes under the bar, and its tooltip on hover -- the value, or the caller's own
/// text. The charts' "now" bars call it without text, so only a direct call reaches these.

#include "UI/Widgets.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <string>

namespace UI::Widgets
{
namespace
{

class VerticalBarTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window running @p body; the text it logged (tooltips included).
    static std::string runFrame(const std::function<void()>& body)
    {
        std::string text;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(400.0F, 400.0F));
        ImGui::Begin("Bars", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        body();
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    }

    static constexpr float BAR_HEIGHT = 120.0F;
    static constexpr float BAR_WIDTH = 40.0F;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(VerticalBarTest, TheValueAndLabelAreWrittenUnderTheBar)
{
    // The same bar with and without its two lines of text: the text adds glyphs to the draw list.
    int withoutText = 0;
    int withText = 0;
    static_cast<void>(runFrame(
        [&]
        {
            const int before = ImGui::GetWindowDrawList()->VtxBuffer.Size;
            drawVerticalBarWithValue("##Bare", 0.5F, ImVec4(0.2F, 0.6F, 1.0F, 1.0F), BAR_HEIGHT, BAR_WIDTH, "", "");
            withoutText = ImGui::GetWindowDrawList()->VtxBuffer.Size - before;
            const int middle = ImGui::GetWindowDrawList()->VtxBuffer.Size;
            drawVerticalBarWithValue("##Labelled", 0.5F, ImVec4(0.2F, 0.6F, 1.0F, 1.0F), BAR_HEIGHT, BAR_WIDTH, "42%", "CPU");
            withText = ImGui::GetWindowDrawList()->VtxBuffer.Size - middle;
        }));
    EXPECT_GT(withText, withoutText);
}

TEST_F(VerticalBarTest, HoveringShowsTheValueOrTheCallersTooltip)
{
    const auto hover = [](const char* tooltipText) -> std::string
    {
        ImVec2 centre;
        const auto draw = [&]
        {
            drawVerticalBarWithValue("##Bar", 0.25F, ImVec4(1.0F, 0.5F, 0.0F, 1.0F), BAR_HEIGHT, BAR_WIDTH, "42%", "CPU", tooltipText);
            centre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5F,
                            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5F);
        };
        static_cast<void>(runFrame(draw));
        ImGui::GetIO().AddMousePosEvent(centre.x, centre.y);
        static_cast<void>(runFrame(draw));
        return runFrame(draw);
    };
    EXPECT_TRUE(hover(nullptr).contains("42%")); // no tooltip text: the value is the tooltip
    const std::string own = hover("CPU: 42% of 4 cores");
    EXPECT_TRUE(own.contains("CPU: 42% of 4 cores")) << own;
}

} // namespace
} // namespace UI::Widgets
