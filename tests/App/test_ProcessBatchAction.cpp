/// @file test_ProcessBatchAction.cpp
/// @brief Batch process actions over the Processes table's selection (#804): resolving the selection
/// to targets by identity, the confirmation's text, the run through a mock IProcessActions (each
/// target's PID and start time passed through, TaskSmack itself last), and the one-line summary.

#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessSelection.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace App::ProcessBatch
{

namespace
{

/// Records every target in call order and fails the PIDs in `failing` with "Operation not permitted".
class RecordingProcessActions : public TestMocks::MockProcessActions
{
  public:
    std::vector<Platform::ProcessTarget> calls;
    std::set<std::int32_t> failing;

    [[nodiscard]] Platform::ProcessActionResult kill(const Platform::ProcessTarget& target) override
    {
        return record(target);
    }
    [[nodiscard]] Platform::ProcessActionResult terminate(const Platform::ProcessTarget& target) override
    {
        return record(target);
    }
    [[nodiscard]] Platform::ProcessActionResult stop(const Platform::ProcessTarget& target) override
    {
        return record(target);
    }
    /// Also records each nice value set (#1484), in call order with `calls`.
    std::vector<std::int32_t> nices;
    [[nodiscard]] Platform::ProcessActionResult setPriority(const Platform::ProcessTarget& target, std::int32_t nice) override
    {
        nices.push_back(nice);
        return record(target);
    }

  private:
    Platform::ProcessActionResult record(const Platform::ProcessTarget& target)
    {
        calls.push_back(target);
        return failing.contains(target.pid) ? Platform::ProcessActionResult::error("Operation not permitted")
                                            : Platform::ProcessActionResult::ok();
    }
};

using Detail::ProcessAction;

constexpr std::int32_t OWN_PID = 4000;

Domain::ProcessSnapshot snapshot(std::int32_t pid, std::uint64_t startTicks, std::uint64_t key, std::string name)
{
    Domain::ProcessSnapshot snap;
    snap.pid = pid;
    snap.startTimeTicks = startTicks;
    snap.uniqueKey = key;
    snap.name = std::move(name);
    return snap;
}

BatchTarget target(std::int32_t pid, std::string name)
{
    return {.target = {.pid = pid, .startTimeTicks = static_cast<std::uint64_t>(pid) * 10U}, .name = std::move(name)};
}

std::vector<BatchTarget> numberedTargets(std::size_t count)
{
    std::vector<BatchTarget> targets;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto pid = static_cast<std::int32_t>(100 + i);
        targets.push_back(target(pid, std::format("proc{}", i)));
    }
    return targets;
}

// ========== Resolving the selection ==========

/// resolveTargets() over @p snaps for the selection @p selected, as ProcessesPanel asks it: an O(1)
/// lookup of each snapshot's exact identity.
std::vector<BatchTarget> resolveSelected(const std::vector<Domain::ProcessSnapshot>& snaps, const ProcessSelection::IdentitySet& selected)
{
    return resolveTargets(snaps, [&](const Platform::ProcessTarget& id) { return selected.contains(id); });
}

TEST(ProcessBatchActionTest, ResolvesSelectedProcessesToTargetsWithTheirStartTimes)
{
    const std::vector<Domain::ProcessSnapshot> snaps{
        snapshot(10, 1000, 501, "a"), snapshot(11, 1100, 502, "b"), snapshot(12, 1200, 503, "c")};
    const ProcessSelection::IdentitySet selected{
        {.pid = 12, .startTimeTicks = 1200}, {.pid = 10, .startTimeTicks = 1000}, {.pid = 99, .startTimeTicks = 9900}}; // 99 has exited
    const std::vector<BatchTarget> targets = resolveSelected(snaps, selected);
    ASSERT_EQ(targets.size(), 2U);
    EXPECT_EQ(targets[0].target.pid, 10);
    EXPECT_EQ(targets[0].target.startTimeTicks, 1000U);
    EXPECT_EQ(targets[0].name, "a");
    EXPECT_EQ(targets[1].target.pid, 12);
    EXPECT_EQ(targets[1].target.startTimeTicks, 1200U);
}

TEST(ProcessBatchActionTest, AReusedPidIsNotResolvedFromTheOldSelection)
{
    // PID 10 was selected with start time 1000; it exited and a new process got PID 10.
    const std::vector<Domain::ProcessSnapshot> snaps{snapshot(10, 9999, 777, "newcomer")};
    const ProcessSelection::IdentitySet selected{{.pid = 10, .startTimeTicks = 1000}};
    EXPECT_TRUE(resolveSelected(snaps, selected).empty());
}

// Forced uniqueKey collisions (#1503): the hash is never what is matched, so a colliding process is
// never resolved in place of, or alongside, the selected one.

TEST(ProcessBatchActionTest, ACollidingLiveProcessIsNotResolvedButTheSelectedOneIs)
{
    // Two different live processes share key 501; only PID 10 was selected. It alone is acted on.
    const std::vector<Domain::ProcessSnapshot> snaps{
        snapshot(10, 1000, 501, "selected"), snapshot(20, 2000, 501, "collides"), snapshot(30, 3000, 503, "other")};
    const ProcessSelection::IdentitySet selected{{.pid = 10, .startTimeTicks = 1000}, {.pid = 30, .startTimeTicks = 3000}};
    const std::vector<BatchTarget> targets = resolveSelected(snaps, selected);
    ASSERT_EQ(targets.size(), 2U);
    EXPECT_EQ(targets[0].target.pid, 10);
    EXPECT_EQ(targets[0].target.startTimeTicks, 1000U);
    EXPECT_EQ(targets[1].target.pid, 30);
}

TEST(ProcessBatchActionTest, ACollidingProcessThatAppearsAfterTheSelectedOneExitsIsNotResolved)
{
    // PID 10 (key 501) was selected and has exited; later processes carry the same key 501 -- one
    // under another PID, one under PID 10 itself (a reused PID whose hash also collides).
    const std::vector<Domain::ProcessSnapshot> snaps{snapshot(20, 2000, 501, "newcomer"), snapshot(10, 5000, 501, "reused")};
    const ProcessSelection::IdentitySet selected{{.pid = 10, .startTimeTicks = 1000}};
    EXPECT_TRUE(resolveSelected(snaps, selected).empty());
}

// ========== Confirmation text ==========

TEST(ProcessBatchActionTest, TitleCountsTheProcesses)
{
    EXPECT_EQ(confirmTitle(ProcessAction::Kill, 5), "Kill 5 processes?");
    EXPECT_EQ(confirmTitle(ProcessAction::Stop, 2), "Suspend 2 processes?");
}

TEST(ProcessBatchActionTest, BodyStatesWhatEachActionDoes)
{
    const std::vector<BatchTarget> two{target(10, "a"), target(11, "b")};
    EXPECT_TRUE(confirmBody(ProcessAction::Terminate, two, OWN_PID).starts_with("2 processes will be asked to exit."));
    EXPECT_TRUE(confirmBody(ProcessAction::Kill, two, OWN_PID).starts_with("2 processes will end immediately"));
    EXPECT_TRUE(confirmBody(ProcessAction::Stop, two, OWN_PID).starts_with("2 processes will stop running"));
    EXPECT_TRUE(confirmBody(ProcessAction::Resume, two, OWN_PID).starts_with("2 processes will continue running."));
}

TEST(ProcessBatchActionTest, BodyListsUpToTheLimitThenCountsTheRest)
{
    const std::vector<BatchTarget> few = numberedTargets(3);
    const std::string short_ = confirmBody(ProcessAction::Kill, few, OWN_PID);
    EXPECT_TRUE(short_.contains("proc0 (PID 100)"));
    EXPECT_TRUE(short_.contains("proc2 (PID 102)"));
    EXPECT_FALSE(short_.contains("more"));

    const std::vector<BatchTarget> many = numberedTargets(CONFIRM_LIST_LIMIT + 5);
    const std::string body = confirmBody(ProcessAction::Kill, many, OWN_PID);
    EXPECT_TRUE(body.contains(std::format("proc{} (PID {})", CONFIRM_LIST_LIMIT - 1, 100 + CONFIRM_LIST_LIMIT - 1)));
    EXPECT_FALSE(body.contains(std::format("proc{} (PID", CONFIRM_LIST_LIMIT)));
    EXPECT_TRUE(body.contains("and 5 more"));
}

TEST(ProcessBatchActionTest, BodyAlwaysNamesTaskSmackItselfAndPidOne)
{
    // Both at the end of a long selection, past the list limit: they are still named, first.
    std::vector<BatchTarget> targets = numberedTargets(CONFIRM_LIST_LIMIT + 4);
    targets.push_back(target(INIT_PID, "systemd"));
    targets.push_back(target(OWN_PID, "TaskSmack"));
    const std::string body = confirmBody(ProcessAction::Kill, targets, OWN_PID);
    EXPECT_TRUE(body.contains("systemd (PID 1)"));
    EXPECT_TRUE(body.contains(std::format("TaskSmack (PID {})", OWN_PID)));
    EXPECT_TRUE(body.contains(std::format("This includes TaskSmack itself (PID {})", OWN_PID)));
    EXPECT_TRUE(body.contains("This includes PID 1 (systemd), the system's init process."));
    // Two notable names plus CONFIRM_LIST_LIMIT - 2 others: 6 of the 12 others are counted.
    EXPECT_TRUE(body.contains("and 6 more"));
    EXPECT_LT(body.find("systemd (PID 1)"), body.find("proc0 (PID 100)"));
}

TEST(ProcessBatchActionTest, TaskSmackAsPidOneGetsBothWarnings)
{
    // TaskSmack as the init of a PID namespace: it is PID 1 and itself, so both lines are shown.
    std::vector<BatchTarget> targets = numberedTargets(2);
    targets.push_back(target(INIT_PID, "TaskSmack"));
    const std::string body = confirmBody(ProcessAction::Kill, targets, INIT_PID);
    EXPECT_TRUE(body.contains("This includes TaskSmack itself (PID 1)"));
    EXPECT_TRUE(body.contains("This includes PID 1 (TaskSmack), the system's init process."));
}

TEST(ProcessBatchActionTest, NoWarningsWithoutNotableTargetsOrWithAnUnknownOwnPid)
{
    const std::vector<BatchTarget> plain{target(10, "a"), target(11, "b")};
    EXPECT_FALSE(confirmBody(ProcessAction::Kill, plain, OWN_PID).contains("This includes"));

    // TaskSmack's own PID unknown (0): nothing is mistaken for it.
    EXPECT_FALSE(isNotableTarget(0, 0));
    EXPECT_FALSE(isNotableTarget(OWN_PID, 0));
    EXPECT_TRUE(isNotableTarget(INIT_PID, 0));
}

// ========== Running the batch ==========

TEST(ProcessBatchActionTest, RunsEveryTargetByIdentity)
{
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets{target(10, "a"), target(11, "b"), target(12, "c")};
    const BatchResult result = runBatchAction(actions, ProcessAction::Kill, targets, OWN_PID);
    EXPECT_EQ(result.attempted, 3U);
    EXPECT_EQ(result.succeeded, 3U);
    EXPECT_EQ(result.failed(), 0U);
    ASSERT_EQ(actions.calls.size(), 3U);
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        EXPECT_EQ(actions.calls[i].pid, targets[i].target.pid);
        EXPECT_EQ(actions.calls[i].startTimeTicks, targets[i].target.startTimeTicks);
    }
}

TEST(ProcessBatchActionTest, DispatchesTheRequestedAction)
{
    TestMocks::MockProcessActions mock;
    const std::vector<BatchTarget> targets{target(10, "a"), target(11, "b")};
    (void) runBatchAction(mock, ProcessAction::Resume, targets, OWN_PID);
    EXPECT_EQ(mock.resumeCount(), 2);
    EXPECT_EQ(mock.killCount(), 0);
    (void) runBatchAction(mock, ProcessAction::Terminate, targets, OWN_PID);
    EXPECT_EQ(mock.terminateCount(), 2);
}

TEST(ProcessBatchActionTest, TaskSmackItselfGoesLast)
{
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets{target(10, "a"), target(OWN_PID, "TaskSmack"), target(12, "c")};
    (void) runBatchAction(actions, ProcessAction::Kill, targets, OWN_PID);
    ASSERT_EQ(actions.calls.size(), 3U);
    EXPECT_EQ(actions.calls[0].pid, 10);
    EXPECT_EQ(actions.calls[1].pid, 12);
    EXPECT_EQ(actions.calls[2].pid, OWN_PID);
}

TEST(ProcessBatchActionTest, KeepsGoingPastFailuresAndQuotesTheFirstFew)
{
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets = numberedTargets(8);
    actions.failing = {101, 102, 104, 106, 107};
    const BatchResult result = runBatchAction(actions, ProcessAction::Kill, targets, OWN_PID);
    EXPECT_EQ(actions.calls.size(), 8U);
    EXPECT_EQ(result.attempted, 8U);
    EXPECT_EQ(result.succeeded, 3U);
    EXPECT_EQ(result.failed(), 5U);
    ASSERT_EQ(result.firstFailures.size(), RESULT_FAILURE_LIMIT);
    EXPECT_EQ(result.firstFailures[0].pid, 101);
    EXPECT_EQ(result.firstFailures[0].name, "proc1");
    EXPECT_EQ(result.firstFailures[0].error, "Operation not permitted");
}

// ========== Result line ==========

TEST(ProcessBatchActionTest, AllSucceededIsOk)
{
    const BatchResult result{.attempted = 4, .succeeded = 4, .firstFailures = {}};
    const Detail::ActionResultMessage msg = formatBatchResultMessage(ProcessAction::Stop, result);
    EXPECT_TRUE(msg.ok);
    EXPECT_EQ(msg.text, "Suspend sent to 4 processes");
}

TEST(ProcessBatchActionTest, PartialFailureCountsAndQuotes)
{
    const BatchResult result{.attempted = 5,
                             .succeeded = 3,
                             .firstFailures = {{.pid = 1, .name = "systemd", .error = "Operation not permitted"},
                                               {.pid = 77, .name = "x", .error = "No such process"}}};
    const Detail::ActionResultMessage msg = formatBatchResultMessage(ProcessAction::Kill, result);
    EXPECT_FALSE(msg.ok);
    EXPECT_EQ(msg.text, "Kill sent to 3 of 5 processes; 2 failed: PID 1 (systemd): Operation not permitted; PID 77 (x): No such process");
}

TEST(ProcessBatchActionTest, AllFailedCountsTheUnquotedOnes)
{
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets = numberedTargets(5);
    actions.failing = {100, 101, 102, 103, 104};
    const BatchResult result = runBatchAction(actions, ProcessAction::Terminate, targets, OWN_PID);
    const Detail::ActionResultMessage msg = formatBatchResultMessage(ProcessAction::Terminate, result);
    EXPECT_FALSE(msg.ok);
    EXPECT_TRUE(msg.text.starts_with("Could not terminate 5 processes: PID 100 (proc0): Operation not permitted; "));
    EXPECT_TRUE(msg.text.ends_with("; and 2 more"));
}

// ========== Batch priority (#1484) ==========

TEST(ProcessBatchActionTest, BatchPriorityIsOfferedOnlyForASelectionWhereThePlatformCanSetIt)
{
    constexpr Platform::ProcessActionCapabilities CAN{.canSetPriority = true};
    constexpr Platform::ProcessActionCapabilities CANNOT{.canTerminate = true, .canKill = true, .canSetPriority = false};
    EXPECT_TRUE(offersBatchPriority(CAN, 2));
    EXPECT_TRUE(offersBatchPriority(CAN, 50));
    EXPECT_FALSE(offersBatchPriority(CAN, 0)); // A single row: Process Details' control
    EXPECT_FALSE(offersBatchPriority(CAN, 1));
    EXPECT_FALSE(offersBatchPriority(CANNOT, 5));
}

TEST(ProcessBatchActionTest, PriorityValueTextStatesWhatIsApplied)
{
    // Linux: the label and the nice value; Windows: the class the nice value maps to.
    EXPECT_EQ(priorityValueText(10, false), "Below Normal (nice: 10)");
    EXPECT_EQ(priorityValueText(-15, false), "High (nice: -15)");
    EXPECT_EQ(priorityValueText(10, true), "Below Normal");
    EXPECT_EQ(priorityValueText(-7, true), "Above Normal");
    // Held to the nice range, as runBatchPriority() holds it.
    EXPECT_EQ(priorityValueText(99, false), "Idle (nice: 19)");
}

TEST(ProcessBatchActionTest, PriorityConfirmTitleAndBody)
{
    EXPECT_EQ(priorityConfirmTitle(5), "Set priority for 5 processes?");
    EXPECT_EQ(priorityConfirmTitle(1), "Set priority for 1 process?");

    const std::vector<BatchTarget> two{target(10, "a"), target(11, "b")};
    const std::string linuxBody = priorityConfirmBody(10, two, OWN_PID, false);
    EXPECT_TRUE(linuxBody.starts_with("The priority of 2 processes will be set to Below Normal (nice: 10)."));
    EXPECT_TRUE(linuxBody.contains("a (PID 10)"));
    EXPECT_TRUE(linuxBody.contains("b (PID 11)"));
    EXPECT_FALSE(linuxBody.contains("root"));
    EXPECT_TRUE(priorityConfirmBody(10, two, OWN_PID, true).starts_with("The priority of 2 processes will be set to Below Normal."));
}

TEST(ProcessBatchActionTest, PriorityConfirmBodyNamesTaskSmackAndPidOneAndWarnsAboutRaising)
{
    std::vector<BatchTarget> targets = numberedTargets(CONFIRM_LIST_LIMIT + 3);
    targets.push_back(target(INIT_PID, "systemd"));
    targets.push_back(target(OWN_PID, "TaskSmack"));
    const std::string body = priorityConfirmBody(-5, targets, OWN_PID, false);
    EXPECT_TRUE(body.contains(std::format("This includes TaskSmack itself (PID {}). It is changed last.", OWN_PID)));
    EXPECT_TRUE(body.contains("This includes PID 1 (systemd), the system's init process."));
    EXPECT_TRUE(body.contains("and 5 more"));
    // A nice value below 0 needs root on Linux; Windows sets classes and has no such line.
    EXPECT_TRUE(body.contains("usually needs root"));
    EXPECT_FALSE(priorityConfirmBody(-15, targets, OWN_PID, true).contains("root"));
}

TEST(ProcessBatchActionTest, PriorityRunsEveryTargetByIdentityWithTheValueAndTaskSmackLast)
{
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets{target(10, "a"), target(OWN_PID, "TaskSmack"), target(12, "c")};
    const BatchResult result = runBatchPriority(actions, targets, 7, OWN_PID);
    EXPECT_EQ(result.attempted, 3U);
    EXPECT_EQ(result.succeeded, 3U);
    ASSERT_EQ(actions.calls.size(), 3U);
    EXPECT_EQ(actions.calls[0].pid, 10);
    EXPECT_EQ(actions.calls[0].startTimeTicks, 100U);
    EXPECT_EQ(actions.calls[1].pid, 12);
    EXPECT_EQ(actions.calls[1].startTimeTicks, 120U);
    EXPECT_EQ(actions.calls[2].pid, OWN_PID);
    EXPECT_EQ(actions.calls[2].startTimeTicks, static_cast<std::uint64_t>(OWN_PID) * 10U);
    EXPECT_EQ(actions.nices, (std::vector<std::int32_t>{7, 7, 7}));
}

TEST(ProcessBatchActionTest, PriorityIsHeldToTheNiceRangeAndSendsNoOtherAction)
{
    TestMocks::MockProcessActions mock;
    const std::vector<BatchTarget> targets{target(10, "a"), target(11, "b")};
    (void) runBatchPriority(mock, targets, -100, OWN_PID);
    EXPECT_EQ(mock.setPriorityCount(), 2);
    EXPECT_EQ(mock.lastSetPriorityNice(), -20);
    EXPECT_EQ(mock.lastSetPriorityPid(), 11);
    EXPECT_EQ(mock.killCount(), 0);
    EXPECT_EQ(mock.terminateCount(), 0);
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
}

TEST(ProcessBatchActionTest, PriorityResultLineNamesTheAppliedValue)
{
    const BatchResult all{.attempted = 4, .succeeded = 4, .firstFailures = {}};
    const Detail::ActionResultMessage ok = formatBatchPriorityResultMessage(10, all, false);
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.text, "Priority set to Below Normal (nice: 10) for 4 processes");
    EXPECT_EQ(formatBatchPriorityResultMessage(10, all, true).text, "Priority set to Below Normal for 4 processes");
}

TEST(ProcessBatchActionTest, PriorityPartialFailuresReadWell)
{
    // Lowering nice below 0 without privileges: some refuse, the rest are changed.
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets = numberedTargets(6);
    actions.failing = {101, 103, 104, 105};
    const BatchResult result = runBatchPriority(actions, targets, -7, OWN_PID);
    EXPECT_EQ(actions.calls.size(), 6U);
    EXPECT_EQ(result.succeeded, 2U);
    const Detail::ActionResultMessage msg = formatBatchPriorityResultMessage(-7, result, false);
    EXPECT_FALSE(msg.ok);
    EXPECT_EQ(msg.text,
              "Priority set to Above Normal (nice: -7) for 2 of 6 processes; 4 failed: PID 101 (proc1): Operation not permitted; "
              "PID 103 (proc3): Operation not permitted; PID 104 (proc4): Operation not permitted; and 1 more");
}

TEST(ProcessBatchActionTest, PriorityAllFailedSaysWhatCouldNotBeSet)
{
    RecordingProcessActions actions;
    const std::vector<BatchTarget> targets = numberedTargets(2);
    actions.failing = {100, 101};
    const BatchResult result = runBatchPriority(actions, targets, -15, OWN_PID);
    const Detail::ActionResultMessage msg = formatBatchPriorityResultMessage(-15, result, true);
    EXPECT_FALSE(msg.ok);
    EXPECT_EQ(msg.text,
              "Could not set priority to High for 2 processes: PID 100 (proc0): Operation not permitted; PID 101 (proc1): Operation not "
              "permitted");
}

} // namespace
} // namespace App::ProcessBatch
