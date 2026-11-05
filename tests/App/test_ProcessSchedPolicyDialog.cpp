/// @file test_ProcessSchedPolicyDialog.cpp
/// @brief The Set scheduling policy... dialog (#3086): which policy it chooses when it opens, that it offers
/// only Normal, Batch and Idle, and, headless, that Apply hands on the chosen policy once and closes, that
/// Apply with nothing chosen does nothing, and that Cancel, Escape and a dismissal hand on nothing.

#include "App/Panels/ProcessSchedPolicyDialog.h"
#include "Platform/IProcessActions.h"
#include "Platform/SchedPolicy.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID()

#include <optional>
#include <string>

namespace App
{
namespace
{

using Platform::SchedPolicy;

constexpr const char* POPUP_ID = "###SetSchedPolicy";

TEST(ProcessSchedPolicyDialogTest, ChoosesTheCurrentPolicyWhenItOffersIt)
{
    ProcessSchedPolicyDialog dialog;
    EXPECT_FALSE(dialog.isPending());
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", SchedPolicy::Batch);
    EXPECT_TRUE(dialog.isPending());
    EXPECT_EQ(dialog.target().pid, 42);
    EXPECT_EQ(dialog.target().startTimeTicks, 7U);
    EXPECT_EQ(dialog.name(), "server");
    EXPECT_EQ(dialog.current(), SchedPolicy::Batch);
    EXPECT_EQ(dialog.choice(), SchedPolicy::Batch);
    EXPECT_TRUE(dialog.canApply());

    dialog.choose(SchedPolicy::Idle);
    EXPECT_EQ(dialog.choice(), SchedPolicy::Idle);
    for (const SchedPolicy unoffered : {SchedPolicy::Fifo, SchedPolicy::RoundRobin, SchedPolicy::Deadline, SchedPolicy::Unknown})
    {
        dialog.choose(unoffered); // not offered: ignored
        EXPECT_EQ(dialog.choice(), SchedPolicy::Idle);
    }
}

// A real-time, Deadline or unread process opens with nothing chosen: Apply waits for the user's choice.
TEST(ProcessSchedPolicyDialogTest, APolicyItDoesntOfferOpensWithNothingChosen)
{
    for (const SchedPolicy current : {SchedPolicy::Fifo, SchedPolicy::RoundRobin, SchedPolicy::Deadline, SchedPolicy::Unknown})
    {
        ProcessSchedPolicyDialog dialog;
        dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", current);
        EXPECT_FALSE(dialog.choice().has_value()) << static_cast<int>(current);
        EXPECT_FALSE(dialog.canApply());
        dialog.choose(SchedPolicy::Normal);
        EXPECT_EQ(dialog.choice(), SchedPolicy::Normal);
    }
}

// Apply may take real-time threads off real time only when the dialog warned that the process runs real time or
// Deadline; opened on Normal, Batch, Idle or an unread policy, it keeps them, whatever the process turns into.
TEST(ProcessSchedPolicyDialogTest, ApplyChangesRealTimeThreadsOnlyAfterTheWarning)
{
    for (const SchedPolicy current : {SchedPolicy::Fifo, SchedPolicy::RoundRobin, SchedPolicy::Deadline})
    {
        ProcessSchedPolicyDialog dialog;
        dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", current);
        EXPECT_EQ(dialog.realtimeThreads(), Platform::RealtimeThreads::Change) << static_cast<int>(current);
    }
    for (const SchedPolicy current : {SchedPolicy::Normal, SchedPolicy::Batch, SchedPolicy::Idle, SchedPolicy::Unknown})
    {
        ProcessSchedPolicyDialog dialog;
        dialog.open({.pid = 42, .startTimeTicks = 7}, "server", current);
        EXPECT_EQ(dialog.realtimeThreads(), Platform::RealtimeThreads::Keep) << static_cast<int>(current);
    }
}

TEST(ProcessSchedPolicyDialogTest, OffersNormalBatchAndIdleEachWithALine)
{
    ASSERT_EQ(SCHED_POLICY_CHOICES.size(), 3U);
    EXPECT_EQ(SCHED_POLICY_CHOICES[0].policy, SchedPolicy::Normal);
    EXPECT_EQ(SCHED_POLICY_CHOICES[1].policy, SchedPolicy::Batch);
    EXPECT_EQ(SCHED_POLICY_CHOICES[2].policy, SchedPolicy::Idle);
    for (const SchedPolicyChoice& choice : SCHED_POLICY_CHOICES)
    {
        EXPECT_TRUE(Platform::isSettableSchedPolicy(choice.policy));
        EXPECT_EQ(std::string(choice.label), Platform::schedPolicyLabel(choice.policy, 0)) << "worded as the Policy column words it";
        EXPECT_FALSE(std::string(choice.description).empty());
    }
}

TEST(ProcessSchedPolicyDialogTest, TheResultLineNamesTheProcessAndPolicy)
{
    const auto ok = schedPolicyResultMessage("server", 42, SchedPolicy::Batch, Platform::ProcessActionResult::ok());
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.text, "Scheduling policy of server (PID 42) set to Batch");

    const auto failed =
        schedPolicyResultMessage("server", 42, SchedPolicy::Idle, Platform::ProcessActionResult::denied("Permission denied"));
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.text, "Permission denied");
}

class ProcessSchedPolicyDialogRenderTest : public ::testing::Test
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

    /// One frame of a window standing in for the Processes panel, drawing @p dialog: what render() returned,
    /// with the text it drew in @p text when given.
    static std::optional<SchedPolicy> frame(ProcessSchedPolicyDialog& dialog, std::string* text = nullptr)
    {
        std::optional<SchedPolicy> applied;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (text != nullptr)
        {
            ImGui::LogToBuffer();
        }
        applied = dialog.render();
        if (text != nullptr)
        {
            *text = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        ImGui::End();
        ImGui::Render();
        return applied;
    }

    /// Presses the top-most modal's item @p label (a filled button: the primary action) on the next frame.
    static void press(const char* label, bool filled)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        const ImGuiID id = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, id) : id);
    }

    /// Frames until render() hands on a policy, at most a few.
    static std::optional<SchedPolicy> applyFrames(ProcessSchedPolicyDialog& dialog)
    {
        std::optional<SchedPolicy> applied;
        for (int i = 0; i < 3 && !applied.has_value(); ++i)
        {
            applied = frame(dialog);
        }
        return applied;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessSchedPolicyDialogRenderTest, ApplyHandsOnTheChosenPolicyOnceAndCloses)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", SchedPolicy::Normal);
    std::string text;
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(frame(dialog, &text).has_value());
    ASSERT_TRUE(ImGui::IsPopupOpen(POPUP_ID, ImGuiPopupFlags_AnyPopupId));
    for (const SchedPolicyChoice& choice : SCHED_POLICY_CHOICES)
    {
        EXPECT_TRUE(text.contains(choice.description)) << text;
    }
    EXPECT_FALSE(text.contains("FIFO")) << "real time isn't offered (#3219)";

    press("Batch", false); // its radio button
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_EQ(dialog.choice(), SchedPolicy::Batch);

    press("Apply", true);
    const std::optional<SchedPolicy> applied = applyFrames(dialog);
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied.value_or(SchedPolicy::Unknown), SchedPolicy::Batch);
    EXPECT_FALSE(frame(dialog).has_value()) << "handed on once";
    EXPECT_FALSE(dialog.isPending());
}

// A real-time process: the dialog says what it runs under, and Apply does nothing until a policy is chosen.
TEST_F(ProcessSchedPolicyDialogRenderTest, ARealTimeProcessIsNamedAndApplyWaitsForAChoice)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", SchedPolicy::Fifo);
    std::string text;
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("real-time FIFO policy")) << text;

    press("Apply", true);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_TRUE(dialog.isPending()) << "still open, waiting for a choice";

    press("Idle", false);
    static_cast<void>(frame(dialog));
    press("Apply", true);
    EXPECT_EQ(applyFrames(dialog), SchedPolicy::Idle);
}

TEST_F(ProcessSchedPolicyDialogRenderTest, CancelEscapeAndADismissalHandOnNothing)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", SchedPolicy::Normal);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    dialog.choose(SchedPolicy::Idle);
    press("Cancel", false);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.isPending());

    // Escape is Cancel (#1944).
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", SchedPolicy::Normal);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    ASSERT_TRUE(dialog.isPending());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_FALSE(frame(dialog).has_value());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.isPending());

    // The selection moved on: closed unapplied.
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", SchedPolicy::Normal);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    dialog.dismiss();
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.isPending());
    EXPECT_FALSE(ImGui::IsPopupOpen(POPUP_ID, ImGuiPopupFlags_AnyPopupId));

    // A request dismissed before it opened is dropped.
    dialog.open({.pid = 42, .startTimeTicks = 7}, "server", SchedPolicy::Normal);
    dialog.dismiss();
    EXPECT_FALSE(dialog.isPending());
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(ImGui::IsPopupOpen(POPUP_ID, ImGuiPopupFlags_AnyPopupId));
}

} // namespace
} // namespace App
