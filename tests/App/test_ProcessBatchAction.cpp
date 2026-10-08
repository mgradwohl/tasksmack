/// @file test_ProcessBatchAction.cpp
/// @brief Batch process actions over the Processes table's selection (#804): resolving the selection
/// to targets by identity, the confirmation's text, the run through a mock IProcessActions (each
/// target's PID and start time passed through, TaskSmack itself last), and the one-line summary.

#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <unordered_set>
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

TEST(ProcessBatchActionTest, ResolvesSelectedKeysToTargetsWithTheirStartTimes)
{
    const std::vector<Domain::ProcessSnapshot> snaps{
        snapshot(10, 1000, 501, "a"), snapshot(11, 1100, 502, "b"), snapshot(12, 1200, 503, "c")};
    const std::unordered_set<std::uint64_t> selected{503, 501, 999}; // 999 has exited
    const std::vector<BatchTarget> targets = resolveTargets(snaps, [&](std::uint64_t key) { return selected.contains(key); });
    ASSERT_EQ(targets.size(), 2U);
    EXPECT_EQ(targets[0].target.pid, 10);
    EXPECT_EQ(targets[0].target.startTimeTicks, 1000U);
    EXPECT_EQ(targets[0].name, "a");
    EXPECT_EQ(targets[1].target.pid, 12);
    EXPECT_EQ(targets[1].target.startTimeTicks, 1200U);
}

TEST(ProcessBatchActionTest, AReusedPidIsNotResolvedFromTheOldSelection)
{
    // PID 10 was selected as key 501; it exited and a new process got PID 10 (key 777).
    const std::vector<Domain::ProcessSnapshot> snaps{snapshot(10, 9999, 777, "newcomer")};
    const std::unordered_set<std::uint64_t> selected{501};
    EXPECT_TRUE(resolveTargets(snaps, [&](std::uint64_t key) { return selected.contains(key); }).empty());
}

TEST(ProcessBatchActionTest, AKeySharedByTwoLiveProcessesResolvesNeither)
{
    // A uniqueKey hash collision: two different live processes carry the selected key. Neither is
    // acted on, so the batch cannot reach a process the user did not select.
    const std::vector<Domain::ProcessSnapshot> snaps{
        snapshot(10, 1000, 501, "selected"), snapshot(20, 2000, 501, "collides"), snapshot(30, 3000, 503, "other")};
    const std::unordered_set<std::uint64_t> selected{501, 503};
    const std::vector<BatchTarget> targets = resolveTargets(snaps, [&](std::uint64_t key) { return selected.contains(key); });
    ASSERT_EQ(targets.size(), 1U);
    EXPECT_EQ(targets[0].target.pid, 30);
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

} // namespace
} // namespace App::ProcessBatch
