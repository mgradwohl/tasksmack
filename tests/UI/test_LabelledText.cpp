/// @file test_LabelledText.cpp
/// @brief A value with a label or note around it (#4306): drawn headless, a value far past ImGui's 3 KB
/// temp buffer, with a multibyte character across the buffer's edge, reaches the screen whole, where
/// ImGui::Text("Label: %s", value) cuts it there, part-way through that character.

#include "UI/LabelledText.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer, ->TempBuffer

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace UI::Widgets
{
namespace
{

constexpr std::string_view LABEL = "Command Line: ";
/// U+65E5, three bytes in UTF-8.
constexpr std::string_view WIDE_CHARACTER = "\xE6\x97\xA5";
constexpr std::string_view END_MARKER = "<end>";

/// About 10 KB of value whose WIDE_CHARACTER starts one byte before ImGui's temp buffer runs out, once
/// LABEL is in front of it, so a cut at the buffer's size splits it.
std::string longValue(std::size_t tempBufferBytes)
{
    // The buffer holds tempBufferBytes - 1 characters and its terminator.
    const std::size_t characterAt = tempBufferBytes - 2 - LABEL.size();
    std::string value(characterAt, 'a');
    value.append(WIDE_CHARACTER);
    value.append(10U * 1024U - value.size(), 'b');
    value.append(END_MARKER);
    return value;
}

// ---- Joining ----

TEST(LabelledTextJoinTest, JoinsPrefixValueAndSuffixInOrder)
{
    std::string scratch;
    EXPECT_EQ(joinLabelledText(scratch, "Command: ", "app.exe", " (deleted)"), "Command: app.exe (deleted)");
    EXPECT_EQ(joinLabelledText(scratch, {}, "app.exe"), "app.exe") << "the last join's text is gone";
    EXPECT_EQ(joinLabelledText(scratch, {}, {}), "");
}

TEST(LabelledTextJoinTest, KeepsTheValueWholeAndByteForByte)
{
    std::string scratch;
    const std::string value = longValue(3073);
    const std::string_view joined = joinLabelledText(scratch, LABEL, value);
    ASSERT_EQ(joined.size(), LABEL.size() + value.size());
    EXPECT_EQ(joined.substr(LABEL.size()), value);
}

// ---- Drawn headless ----

class LabelledTextRenderTest : public ::testing::Test
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

    /// The size of ImGui's temp buffer, which a prefixed "%s" formats into.
    [[nodiscard]] static std::size_t tempBufferBytes()
    {
        return static_cast<std::size_t>(GImGui->TempBuffer.Size);
    }

    /// One frame drawing @p body in a window; returns every text it drew.
    static std::string runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(400.0F, 300.0F));
        ImGui::Begin("Text", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        body();
        std::string logged = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return logged;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(LabelledTextRenderTest, APrefixedFormatCutsALongValueMidCharacter)
{
    // Why the helpers exist: ImGui formats "Label: %s" into its temp buffer, and drops the rest.
    const std::string value = longValue(tempBufferBytes());
    const std::string logged = runFrame([&] { ImGui::Text("Command Line: %s", value.c_str()); });
    EXPECT_EQ(logged.find(END_MARKER), std::string::npos) << "ImGui no longer cuts at its temp buffer";
    EXPECT_NE(logged.find(WIDE_CHARACTER.substr(0, 1)), std::string::npos) << "the cut keeps the character's first byte";
    EXPECT_EQ(logged.find(WIDE_CHARACTER), std::string::npos) << "and drops the rest of it";
}

TEST_F(LabelledTextRenderTest, EveryVariantDrawsALongValueWhole)
{
    const std::string value = longValue(tempBufferBytes());
    std::string whole(LABEL);
    whole.append(value).append(" (deleted)");
    const ImVec4 color(1.0F, 0.0F, 0.0F, 1.0F);
    const std::array<std::function<void()>, 4> variants = {
        [&] { labelledText(LABEL, value, " (deleted)"); },
        [&] { labelledTextColored(color, LABEL, value, " (deleted)"); },
        [&] { labelledTextDisabled(LABEL, value, " (deleted)"); },
        [&] { labelledTextWrapped(LABEL, value, " (deleted)"); },
    };
    for (std::size_t variant = 0; variant < variants.size(); ++variant)
    {
        const std::string logged = runFrame(variants[variant]);
        EXPECT_NE(logged.find(whole), std::string::npos) << "variant " << variant << " lost part of the text";
    }
}

TEST_F(LabelledTextRenderTest, NormalTextDrawsAsTheFormattedCallDid)
{
    const std::string formatted = runFrame([] { ImGui::Text("Command: %s", "C:\\Windows\\System32\\svchost.exe -k netsvcs"); });
    const std::string labelled = runFrame([] { labelledText("Command: ", "C:\\Windows\\System32\\svchost.exe -k netsvcs"); });
    EXPECT_EQ(labelled, formatted);
    EXPECT_EQ(runFrame([] { labelledText({}, ""); }), runFrame([] { ImGui::Text("%s", ""); }));
}

TEST_F(LabelledTextRenderTest, ColouredVariantsUseTheirColourAndRestoreTheStyle)
{
    const ImVec4 color(1.0F, 0.0F, 0.0F, 1.0F);
    ImU32 drawnColor = 0;
    ImVec4 textAfter;
    static_cast<void>(runFrame(
        [&]
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const int before = drawList->VtxBuffer.Size;
            labelledTextColored(color, "Error: ", "access denied");
            ASSERT_GT(drawList->VtxBuffer.Size, before);
            drawnColor = drawList->VtxBuffer[before].col;
            textAfter = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }));
    EXPECT_EQ(drawnColor, ImGui::ColorConvertFloat4ToU32(color));
    const ImVec4 styleText = ImGui::GetStyle().Colors[ImGuiCol_Text];
    EXPECT_FLOAT_EQ(textAfter.x, styleText.x);
    EXPECT_FLOAT_EQ(textAfter.y, styleText.y);
    EXPECT_FLOAT_EQ(textAfter.z, styleText.z);
}

} // namespace
} // namespace UI::Widgets
