/// @file test_ProcessAffinityDialog.cpp
/// @brief The row menu's Set affinity... dialog (#1887): which CPUs it offers and ticks, All/None and a
/// box, and, headless, that Apply hands on the ticked CPUs once and closes, that Apply with nothing
/// ticked does nothing, and that Cancel hands on nothing.

#include "App/Panels/ProcessAffinityDialog.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID()

#include <cstddef>
#include <optional>

namespace App
{
namespace
{

constexpr const char* POPUP_ID = "###SetAffinity";

TEST(ProcessAffinityDialogTest, OffersEveryOnlineCpuAndAnyHigherOneItsAffinityNames)
{
    EXPECT_EQ(affinityDialogCpuCount(Platform::CpuAffinity::fromMask(0b1010U), 8), 8U);
    Platform::CpuAffinity high;
    high.set(70);
    EXPECT_EQ(affinityDialogCpuCount(high, 8), 71U) << "an offline CPU it names keeps its box";
    EXPECT_EQ(affinityDialogCpuCount(Platform::CpuAffinity{}, 0), 1U) << "never no box";
}

TEST(ProcessAffinityDialogTest, TicksTheCurrentAffinityAndAppliesTheBoxes)
{
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b0101U), 4);
    EXPECT_TRUE(dialog.isPending());
    EXPECT_EQ(dialog.cpuCount(), 4U);
    EXPECT_EQ(dialog.target().pid, 42);
    EXPECT_EQ(dialog.name(), "server");
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0b0101U));

    dialog.choose(1, true);
    dialog.choose(0, false);
    dialog.choose(99, true); // not offered: ignored
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0b0110U));
    dialog.chooseAll(false);
    EXPECT_TRUE(dialog.choice().empty());
    dialog.chooseAll(true);
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0b1111U));

    // An affinity that couldn't be read ticks every box.
    dialog.open({.pid = 43, .startTimeTicks = 8}, "other", Platform::CpuAffinity{}, 3);
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0b111U));
}

class ProcessAffinityDialogRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1000.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window standing in for the Processes panel, drawing @p dialog: what render() returned.
    static std::optional<Platform::CpuAffinity> frame(ProcessAffinityDialog& dialog)
    {
        std::optional<Platform::CpuAffinity> applied;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        applied = dialog.render();
        ImGui::End();
        ImGui::Render();
        return applied;
    }

    /// Presses the top-most modal's button @p label (a filled one: the primary action) on the next frame.
    static void press(const char* label, bool filled)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        const ImGuiID id = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, id) : id);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessAffinityDialogRenderTest, ApplyHandsOnTheTickedCpusOnceAndCloses)
{
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b11U), 4);
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(frame(dialog).has_value());
    ASSERT_TRUE(ImGui::IsPopupOpen(POPUP_ID, ImGuiPopupFlags_AnyPopupId));
    dialog.choose(3, true);

    press("Apply", true);
    std::optional<Platform::CpuAffinity> applied;
    for (int i = 0; i < 3 && !applied.has_value(); ++i)
    {
        applied = frame(dialog);
    }
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied.value_or(Platform::CpuAffinity{}), Platform::CpuAffinity::fromMask(0b1011U));
    EXPECT_FALSE(frame(dialog).has_value()) << "handed on once";
    EXPECT_FALSE(dialog.isPending());
}

TEST_F(ProcessAffinityDialogRenderTest, ApplyWithNothingTickedDoesNothingAndCancelHandsOnNothing)
{
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b11U), 4);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    dialog.chooseAll(false);
    press("Apply", true);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_TRUE(dialog.isPending()) << "still open, saying to choose a CPU";

    press("Cancel", false);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.isPending());
}

} // namespace
} // namespace App
