/// @file test_ProcessSchedPolicyDialog.cpp
/// @brief The Set scheduling policy... dialog (#3086, #3219): which policy and priority it chooses when it opens,
/// that it offers Normal, Batch, Idle, FIFO and RR (never Deadline, and no real time for TaskSmack itself), and,
/// headless, that Apply hands on the chosen setting once and closes, that real time shows the slider and the
/// warning and asks to confirm first, that Apply with nothing chosen does nothing, and that Cancel, Escape and a
/// dismissal hand on nothing.

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

    EXPECT_EQ(dialog.setting(), (SchedPolicySetting{.policy = SchedPolicy::Batch, .rtPriority = 0}));
    EXPECT_EQ(dialog.rtPriority(), DEFAULT_REALTIME_PRIORITY) << "the slider starts mid-range for a process not on real time";

    dialog.choose(SchedPolicy::Idle);
    EXPECT_EQ(dialog.choice(), SchedPolicy::Idle);
    for (const SchedPolicy unoffered : {SchedPolicy::Deadline, SchedPolicy::Unknown})
    {
        dialog.choose(unoffered); // not offered: ignored
        EXPECT_EQ(dialog.choice(), SchedPolicy::Idle);
    }
    dialog.choose(SchedPolicy::Fifo); // #3219
    EXPECT_EQ(dialog.setting(), (SchedPolicySetting{.policy = SchedPolicy::Fifo, .rtPriority = DEFAULT_REALTIME_PRIORITY}));
    EXPECT_TRUE(dialog.showsRealtimeWarning());
    dialog.setRtPriority(150);
    EXPECT_EQ(dialog.rtPriority(), 99) << "clamped to 1 to 99";
    dialog.setRtPriority(0);
    EXPECT_EQ(dialog.rtPriority(), 1);
}

// #3219: a real-time process opens with its policy and priority chosen, so Apply re-applies what it runs under.
TEST(ProcessSchedPolicyDialogTest, ARealtimeProcessOpensWithItsPolicyAndPriority)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", SchedPolicy::RoundRobin, 20);
    EXPECT_EQ(dialog.setting(), (SchedPolicySetting{.policy = SchedPolicy::RoundRobin, .rtPriority = 20}));
    EXPECT_TRUE(dialog.showsRealtimeWarning());
    dialog.choose(SchedPolicy::Normal);
    EXPECT_EQ(dialog.setting(), (SchedPolicySetting{.policy = SchedPolicy::Normal, .rtPriority = 0})) << "no priority for Normal";
    EXPECT_FALSE(dialog.showsRealtimeWarning());
    dialog.choose(SchedPolicy::Fifo);
    EXPECT_EQ(dialog.rtPriority(), 20) << "the slider keeps its place";
}

// #3219: a FIFO or RR process whose priority wasn't read (0, which real time never has) opens with nothing chosen
// and the slider at 50, rather than preselecting priority 1, so the user picks explicitly.
TEST(ProcessSchedPolicyDialogTest, ARealtimeProcessWithAnUnreadPriorityOpensWithNothingChosen)
{
    for (const SchedPolicy current : {SchedPolicy::Fifo, SchedPolicy::RoundRobin})
    {
        ProcessSchedPolicyDialog dialog;
        dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", current, 0);
        EXPECT_FALSE(dialog.choice().has_value()) << static_cast<int>(current);
        EXPECT_FALSE(dialog.canApply());
        EXPECT_EQ(dialog.rtPriority(), DEFAULT_REALTIME_PRIORITY);
        dialog.choose(current);
        EXPECT_EQ(dialog.setting(), (SchedPolicySetting{.policy = current, .rtPriority = DEFAULT_REALTIME_PRIORITY}));
    }
}

// #3219: TaskSmack's own process is never offered real time; the time-sharing policies stay.
TEST(ProcessSchedPolicyDialogTest, TaskSmacksOwnProcessIsNeverOfferedRealtime)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "tasksmack", SchedPolicy::Normal, 0, {.ownPid = 42});
    EXPECT_FALSE(dialog.offersRealtime());
    dialog.choose(SchedPolicy::Fifo);
    dialog.choose(SchedPolicy::RoundRobin);
    EXPECT_EQ(dialog.choice(), SchedPolicy::Normal);
    dialog.choose(SchedPolicy::Batch);
    EXPECT_EQ(dialog.choice(), SchedPolicy::Batch);

    dialog.open({.pid = 43, .startTimeTicks = 7}, "server", SchedPolicy::Normal, 0, {.ownPid = 42});
    EXPECT_TRUE(dialog.offersRealtime());
}

// A Deadline or unread process opens with nothing chosen: Apply waits for the user's choice.
TEST(ProcessSchedPolicyDialogTest, APolicyItDoesntOfferOpensWithNothingChosen)
{
    for (const SchedPolicy current : {SchedPolicy::Deadline, SchedPolicy::Unknown})
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

TEST(ProcessSchedPolicyDialogTest, OffersEveryPolicyButDeadlineEachWithALine)
{
    ASSERT_EQ(SCHED_POLICY_CHOICES.size(), 5U);
    EXPECT_EQ(SCHED_POLICY_CHOICES[0].policy, SchedPolicy::Normal);
    EXPECT_EQ(SCHED_POLICY_CHOICES[1].policy, SchedPolicy::Batch);
    EXPECT_EQ(SCHED_POLICY_CHOICES[2].policy, SchedPolicy::Idle);
    EXPECT_EQ(SCHED_POLICY_CHOICES[3].policy, SchedPolicy::Fifo);
    EXPECT_EQ(SCHED_POLICY_CHOICES[4].policy, SchedPolicy::RoundRobin);
    for (const SchedPolicyChoice& choice : SCHED_POLICY_CHOICES)
    {
        EXPECT_TRUE(Platform::isSettableSchedPolicy(choice.policy));
        // Worded as the Policy column words it, without the priority real time adds there ("FIFO 50").
        EXPECT_TRUE(Platform::schedPolicyLabel(choice.policy, 0).starts_with(choice.label)) << choice.label;
        EXPECT_FALSE(std::string(choice.description).empty());
    }
}

TEST(ProcessSchedPolicyDialogTest, TheResultLineNamesTheProcessAndPolicy)
{
    const auto ok = schedPolicyResultMessage("server", 42, {.policy = SchedPolicy::Batch}, Platform::ProcessActionResult::ok());
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.text, "Scheduling policy of server (PID 42) set to Batch");
    const auto realtime =
        schedPolicyResultMessage("audio", 42, {.policy = SchedPolicy::Fifo, .rtPriority = 50}, Platform::ProcessActionResult::ok());
    EXPECT_EQ(realtime.text, "Scheduling policy of audio (PID 42) set to FIFO 50");
    Platform::ProcessActionResult kept = Platform::ProcessActionResult::ok();
    kept.successNote = "2 real-time threads kept their policy";
    EXPECT_EQ(schedPolicyResultMessage("audio", 42, {.policy = SchedPolicy::Batch}, kept).text,
              "Scheduling policy of audio (PID 42) set to Batch; 2 real-time threads kept their policy");

    const auto failed =
        schedPolicyResultMessage("server", 42, {.policy = SchedPolicy::Idle}, Platform::ProcessActionResult::denied("Permission denied"));
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
    static std::optional<SchedPolicySetting> frame(ProcessSchedPolicyDialog& dialog, std::string* text = nullptr)
    {
        std::optional<SchedPolicySetting> applied;
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

    /// Frames until render() hands on a setting, at most a few.
    static std::optional<SchedPolicySetting> applyFrames(ProcessSchedPolicyDialog& dialog)
    {
        std::optional<SchedPolicySetting> applied;
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
    EXPECT_FALSE(text.contains("RT throttling")) << "the real-time warning shows only while FIFO or RR is chosen";
    EXPECT_FALSE(text.contains("Real-time priority")) << "and so does the slider";
    EXPECT_FALSE(text.contains("Deadline")) << "never offered";

    press("Batch", false); // its radio button
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_EQ(dialog.choice(), SchedPolicy::Batch);

    press("Apply", true);
    const std::optional<SchedPolicySetting> applied = applyFrames(dialog);
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied, (SchedPolicySetting{.policy = SchedPolicy::Batch, .rtPriority = 0}));
    EXPECT_FALSE(dialog.confirming()) << "no confirm step for time sharing";
    EXPECT_FALSE(frame(dialog).has_value()) << "handed on once";
    EXPECT_FALSE(dialog.isPending());
}

// #3219: choosing RR shows the slider and the warning; Apply asks to confirm, naming the process, its PID, the
// policy and the priority; Back returns to the choices, and Set hands the setting on.
TEST_F(ProcessSchedPolicyDialogRenderTest, RealtimeWarnsAndAsksToConfirmBeforeItIsHandedOn)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", SchedPolicy::Normal);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    press("RR", false);
    std::string text;
    EXPECT_FALSE(frame(dialog, &text).has_value());
    ASSERT_EQ(dialog.choice(), SchedPolicy::RoundRobin);
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("Real-time priority")) << text;
    EXPECT_TRUE(text.contains("starve everything else")) << text;
    EXPECT_TRUE(text.contains("/proc/sys/kernel/sched_rt_runtime_us")) << text;
    EXPECT_TRUE(text.contains("prefer RR over FIFO")) << text;
    EXPECT_TRUE(text.contains("RLIMIT_RTPRIO")) << "without CAP_SYS_NICE it may be refused: " << text;
    dialog.setRtPriority(30); // as the slider would

    press("Apply", true);
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_TRUE(dialog.confirming()) << "Apply asks first";
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("Set audio (PID 42) to RR 30?")) << text;
    EXPECT_TRUE(text.contains("round robin policy at priority 30")) << text;
    EXPECT_TRUE(text.contains("RLIMIT_RTPRIO")) << "the may-be-refused note is repeated at the confirm: " << text;
    EXPECT_TRUE(text.contains("Authenticate and retry")) << text;

    press("Back", false);
    EXPECT_FALSE(frame(dialog).has_value());
    EXPECT_FALSE(dialog.confirming());
    EXPECT_TRUE(dialog.isPending()) << "back to the choices, still open";
    EXPECT_EQ(dialog.choice(), SchedPolicy::RoundRobin);

    press("Apply", true);
    EXPECT_FALSE(frame(dialog).has_value());
    ASSERT_TRUE(dialog.confirming());
    static_cast<void>(frame(dialog));
    press("Set", true);
    EXPECT_EQ(applyFrames(dialog), (SchedPolicySetting{.policy = SchedPolicy::RoundRobin, .rtPriority = 30}));
    EXPECT_FALSE(dialog.isPending());
    EXPECT_FALSE(dialog.confirming());
}

// #3219: with CAP_SYS_NICE (or the helper running) the may-be-refused line is left out; Escape at the confirm step
// cancels, as it does at the choices.
TEST_F(ProcessSchedPolicyDialogRenderTest, EscapeAtTheConfirmStepCancels)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", SchedPolicy::Fifo, 60, {.realtimeNeedsNoPassword = true});
    std::string text;
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("RT throttling")) << text;
    EXPECT_FALSE(text.contains("RLIMIT_RTPRIO")) << text;
    press("Apply", true);
    static_cast<void>(frame(dialog));
    ASSERT_TRUE(dialog.confirming());
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("Set audio (PID 42) to FIFO 60?")) << text;
    EXPECT_FALSE(text.contains("RLIMIT_RTPRIO")) << "no may-be-refused note where real time needs no password: " << text;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    EXPECT_FALSE(frame(dialog).has_value());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.isPending());
}

// #3219: a real-time process whose priority wasn't read says so, and Apply waits for a choice.
TEST_F(ProcessSchedPolicyDialogRenderTest, ARealtimeProcessWithAnUnreadPriorityIsNamedAndApplyWaits)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", SchedPolicy::Fifo, 0);
    std::string text;
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("real-time priority TaskSmack couldn't read")) << text;
    press("Apply", true);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_FALSE(dialog.confirming());
    EXPECT_TRUE(dialog.isPending());
}

// #3219: TaskSmack's own process draws FIFO and RR disabled, with the reason.
TEST_F(ProcessSchedPolicyDialogRenderTest, TaskSmacksOwnProcessDrawsRealtimeDisabled)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "tasksmack", SchedPolicy::Normal, 0, {.ownPid = 42});
    std::string text;
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("This is TaskSmack itself, which never runs real time")) << text;
    press("FIFO", false); // disabled: the press does nothing
    for (int i = 0; i < 2; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_EQ(dialog.choice(), SchedPolicy::Normal);
}

// A Deadline process: the dialog says what it runs under, and Apply does nothing until a policy is chosen.
TEST_F(ProcessSchedPolicyDialogRenderTest, ADeadlineProcessIsNamedAndApplyWaitsForAChoice)
{
    ProcessSchedPolicyDialog dialog;
    dialog.open({.pid = 42, .startTimeTicks = 7}, "audio", SchedPolicy::Deadline);
    std::string text;
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog, &text));
    EXPECT_TRUE(text.contains("the Deadline policy")) << text;

    press("Apply", true);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(frame(dialog).has_value());
    }
    EXPECT_TRUE(dialog.isPending()) << "still open, waiting for a choice";

    press("Idle", false);
    static_cast<void>(frame(dialog));
    press("Apply", true);
    EXPECT_EQ(applyFrames(dialog), (SchedPolicySetting{.policy = SchedPolicy::Idle, .rtPriority = 0}));
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
