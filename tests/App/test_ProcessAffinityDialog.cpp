/// @file test_ProcessAffinityDialog.cpp
/// @brief The row menu's Set affinity... dialog (#1887): which CPUs it offers and ticks, All/None and a
/// box, and, headless, that Apply hands on the ticked CPUs once and closes, that Apply with nothing
/// ticked does nothing, and that Cancel hands on nothing.

#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessAffinitySections.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID()

#include <cstddef>
#include <optional>
#include <vector>

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

// --- Sections by processor group and kind of core (#1881) ---

[[nodiscard]] Platform::CpuAffinityLayout hybridLayout()
{
    // CPUs 0-3 performance (class 1), 4-7 efficiency (class 0).
    Platform::CpuAffinityLayout layout;
    layout.efficiencyClassByCpu = {1, 1, 1, 1, 0, 0, 0, 0};
    layout.online = Platform::CpuAffinity::fromMask(0xFFU);
    return layout;
}

TEST(ProcessAffinitySectionsTest, OneKindOfCoreInOneGroupIsOneUntitledSection)
{
    const std::vector<std::size_t> cpus = affinityDialogOfferedCpus(Platform::CpuAffinity{}, 4, Platform::CpuAffinityLayout{});
    EXPECT_EQ(cpus, (std::vector<std::size_t>{0, 1, 2, 3}));
    const auto sections = affinityDialogSections(cpus, Platform::CpuAffinityLayout{});
    ASSERT_EQ(sections.size(), 1U);
    EXPECT_TRUE(sections.front().title.empty());
    EXPECT_EQ(sections.front().cpus, cpus);
}

TEST(ProcessAffinitySectionsTest, AHybridCpuListsPerformanceCoresFirst)
{
    Platform::CpuAffinityLayout layout = hybridLayout();
    layout.efficiencyClassByCpu = {0, 0, 1, 1}; // E-cores numbered first, as on some parts
    layout.online = Platform::CpuAffinity::fromMask(0b1111U);
    const auto sections = affinityDialogSections(affinityDialogOfferedCpus(Platform::CpuAffinity{}, 0, layout), layout);
    ASSERT_EQ(sections.size(), 2U);
    EXPECT_EQ(sections[0].title, "Performance cores");
    EXPECT_EQ(sections[0].cpus, (std::vector<std::size_t>{2, 3}));
    EXPECT_EQ(sections[1].title, "Efficiency cores");
    EXPECT_EQ(sections[1].cpus, (std::vector<std::size_t>{0, 1}));
}

TEST(ProcessAffinitySectionsTest, ProcessorGroupsEachGetASectionAndACpuPastThemIsOther)
{
    Platform::CpuAffinityLayout layout;
    layout.groupSizes = {2, 2};
    const std::vector<std::size_t> cpus{0, 1, 2, 3, 9};
    const auto sections = affinityDialogSections(cpus, layout);
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[0].title, "Group 0");
    EXPECT_EQ(sections[0].cpus, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(sections[1].title, "Group 1");
    EXPECT_EQ(sections[1].cpus, (std::vector<std::size_t>{2, 3}));
    EXPECT_EQ(sections[2].title, "Other CPUs");
    EXPECT_EQ(sections[2].cpus, (std::vector<std::size_t>{9}));

    layout.efficiencyClassByCpu = {1, 0, 1, 0};
    const auto both = affinityDialogSections({0, 1, 2, 3}, layout);
    ASSERT_EQ(both.size(), 4U);
    EXPECT_EQ(both[0].title, "Group 0: Performance cores");
    EXPECT_EQ(both[1].title, "Group 0: Efficiency cores");
    EXPECT_EQ(both[2].title, "Group 1: Performance cores");
    EXPECT_EQ(both[3].cpus, (std::vector<std::size_t>{3}));
}

TEST(ProcessAffinitySectionsTest, OffersTheOnlineCpusAndAnyOfflineOneTheAffinityNames)
{
    Platform::CpuAffinityLayout layout;
    layout.online = Platform::CpuAffinity::fromMask(0b1011U); // CPU 2 offline
    EXPECT_EQ(affinityDialogOfferedCpus(Platform::CpuAffinity::fromMask(0b0001U), 16, layout), (std::vector<std::size_t>{0, 1, 3}))
        << "the online set wins over the count";
    EXPECT_EQ(affinityDialogOfferedCpus(Platform::CpuAffinity::fromMask(0b0100U), 16, layout), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(affinityDialogOfferedCpus(Platform::CpuAffinity{}, 0, Platform::CpuAffinityLayout{}), (std::vector<std::size_t>{0}));
}

TEST(ProcessAffinityDialogTest, ASectionsBoxTicksItsCpusAndApplyNeedsOne)
{
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b0001U), 8, hybridLayout());
    ASSERT_EQ(dialog.sections().size(), 2U);
    EXPECT_EQ(dialog.cpuCount(), 8U);
    EXPECT_TRUE(dialog.canApply());

    dialog.chooseSection(1, true); // The efficiency cores
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0xF1U));
    dialog.chooseSection(0, false);
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0xF0U));
    dialog.chooseSection(5, true); // No such section: ignored
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0xF0U));
    dialog.chooseAll(false);
    EXPECT_FALSE(dialog.canApply());
}

TEST(ProcessAffinityDialogTest, ACpuNotOfferedIsNeverTicked)
{
    Platform::CpuAffinityLayout layout;
    layout.online = Platform::CpuAffinity::fromMask(0b1011U); // CPU 2 offline
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity{}, 4, layout);
    EXPECT_EQ(dialog.cpuCount(), 3U);
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0b1011U)) << "an unread affinity ticks every offered box";
    dialog.choose(2, true);
    dialog.chooseAll(true);
    EXPECT_EQ(dialog.choice(), Platform::CpuAffinity::fromMask(0b1011U));
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

// #1891: the dialog's All and None buttons tick and clear every box; Apply then hands on what they left.
TEST_F(ProcessAffinityDialogRenderTest, AllAndNoneTickAndClearEveryBox)
{
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b1U), 4);
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(frame(dialog).has_value());

    press("None", false);
    EXPECT_FALSE(frame(dialog).has_value());
    press("Apply", true); // Nothing ticked: Apply does nothing, the dialog stays
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_TRUE(dialog.isPending());

    press("All", false);
    EXPECT_FALSE(frame(dialog).has_value());
    press("Apply", true);
    std::optional<Platform::CpuAffinity> applied;
    for (int i = 0; i < 3 && !applied.has_value(); ++i)
    {
        applied = frame(dialog);
    }
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied.value_or(Platform::CpuAffinity{}), Platform::CpuAffinity::fromMask(0b1111U)); // CPUs 0-3
}

// #1881, #1891: where the choice is set as CPU sets (Windows with several processor groups), the dialog
// says so, and how many groups there are.
TEST_F(ProcessAffinityDialogRenderTest, ChoicesAppliedAsCpuSetsAreExplained)
{
    ProcessAffinityDialog dialog;
    Platform::CpuAffinityLayout layout;
    layout.groupSizes = {64, 64};
    layout.online = Platform::CpuAffinity::fromMask(0b11U);
    layout.appliedAsCpuSets = true;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b1U), 128, layout);
    std::string text;
    for (int i = 0; i < 3; ++i)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        static_cast<void>(dialog.render());
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }
    EXPECT_TRUE(text.contains("2 processor groups")) << text;
    EXPECT_TRUE(text.contains("default CPU sets")) << text;
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

// Escape is Cancel (#1944): the dialog closes and hands on nothing, whatever was ticked.
TEST_F(ProcessAffinityDialogRenderTest, EscapeCancelsAndHandsOnNothing)
{
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0b11U), 4);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    ASSERT_TRUE(dialog.isPending());
    dialog.choose(3, true);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_FALSE(frame(dialog).has_value());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.isPending());
    EXPECT_FALSE(ImGui::IsPopupOpen(POPUP_ID, ImGuiPopupFlags_AnyPopupId));
}

TEST_F(ProcessAffinityDialogRenderTest, SectionsDrawAndApplyHandsOnASectionsChoice)
{
    Platform::CpuAffinityLayout layout;
    layout.groupSizes = {4, 4};
    layout.efficiencyClassByCpu = {1, 1, 0, 0, 1, 1, 0, 0};
    layout.online = Platform::CpuAffinity::fromMask(0xFFU);
    layout.appliedAsCpuSets = true; // The CPU sets note draws too
    ProcessAffinityDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", Platform::CpuAffinity::fromMask(0x01U), 8, layout);
    ASSERT_EQ(dialog.sections().size(), 4U);
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(frame(dialog).has_value());
    dialog.chooseAll(false);
    EXPECT_FALSE(dialog.canApply()) << "Apply is drawn disabled";
    EXPECT_FALSE(frame(dialog).has_value());
    dialog.chooseSection(2, true); // Group 1's performance cores, CPUs 4-5

    press("Apply", true);
    std::optional<Platform::CpuAffinity> applied;
    for (int i = 0; i < 3 && !applied.has_value(); ++i)
    {
        applied = frame(dialog);
    }
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied.value_or(Platform::CpuAffinity{}), Platform::CpuAffinity::fromMask(0x30U));
}

} // namespace
} // namespace App
