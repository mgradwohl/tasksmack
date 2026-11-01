// Process Details' Threads section (#3069): its cadence, CPU % from two reads, sort, filter and statuses
// against a mock reader, and its real ImGui render, headless.

#include "App/Panels/ProcessThreadsView.h"
#include "App/Panels/ThreadActionControl.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "UI/Format.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::ProcessThread;
using Platform::ThreadsReadResult;
using Platform::ThreadsReadStatus;

constexpr Platform::ProcessTarget TARGET{.pid = 100, .startTimeTicks = 4242};

class MockThreadsReader final : public Platform::IProcessThreadsReader
{
  public:
    ThreadsReadResult result;
    std::atomic<int> reads{0};

    [[nodiscard]] bool hasThreads() const override
    {
        return true;
    }
    [[nodiscard]] ThreadsReadResult readThreads(const Platform::ProcessTarget& /*target*/) override
    {
        ++reads;
        return result;
    }

    // "Authenticate to read" (#3069): whether it is offered, and what the administrator's read answers.
    bool canAuthorize = false;
    ThreadsReadResult authorizedResult;
    std::atomic<int> authorizedReads{0};

    [[nodiscard]] bool canReadAuthorized() const override
    {
        return canAuthorize;
    }
    [[nodiscard]] ThreadsReadResult readAuthorized(const Platform::ProcessTarget& /*target*/, const std::stop_token& /*stopToken*/) override
    {
        ++authorizedReads;
        return authorizedResult;
    }
};

[[nodiscard]] ThreadsReadResult threads(std::vector<ProcessThread> list)
{
    return {.status = ThreadsReadStatus::Ok, .threads = std::move(list), .detail = {}, .ticksPerSecond = 100};
}

[[nodiscard]] ProcessThread thread(std::int32_t tid, const char* name, std::uint64_t ticks, std::uint64_t start = 10)
{
    return {.tid = tid, .name = name, .state = 'S', .userTicks = ticks, .systemTicks = 0, .nice = 0, .startTicks = start};
}

TEST(ProcessThreadsViewTest, CpuPercentIsAShareOfTheWholeMachine)
{
    // 50 ticks at 100 a second is 0.5 s of CPU in 1 s, on 2 CPUs: 25 %.
    EXPECT_DOUBLE_EQ(Detail::threadCpuPercent(50, 100, 1.0, 2).value_or(-1.0), 25.0);
    EXPECT_DOUBLE_EQ(Detail::threadCpuPercent(1000, 100, 1.0, 1).value_or(-1.0), 100.0); // clamped
    EXPECT_FALSE(Detail::threadCpuPercent(5, 100, 0.0, 1).has_value());
    EXPECT_FALSE(Detail::threadCpuPercent(5, 0, 1.0, 1).has_value());
}

TEST(ProcessThreadsViewTest, CpuPercentNeedsTwoReadsOfTheSameThread)
{
    ProcessThreadsView view(2);
    view.applyResult(threads({thread(100, "main", 100), thread(101, "worker", 0)}), 10.0);
    ASSERT_EQ(view.rows().size(), 2U);
    for (const ProcessThreadsView::Row& row : view.rows())
    {
        EXPECT_FALSE(row.cpuPercent.has_value()) << row.thread.name; // the first read: the dash
        EXPECT_EQ(row.cpu, Detail::THREAD_CPU_UNKNOWN);
    }

    // 2 s later: main used 0 ticks more, worker 200 (2 s of CPU on 2 CPUs: 50 %); TID 102 is new, and
    // TID 100 now names a thread that started later (a reused TID): neither has a CPU % yet.
    view.applyResult(threads({thread(100, "main", 5, 99), thread(101, "worker", 200), thread(102, "new", 7)}), 12.0);
    ASSERT_EQ(view.rows().size(), 3U);
    // Sorted by CPU %, highest first, by default.
    EXPECT_EQ(view.rows()[0].thread.tid, 101);
    EXPECT_DOUBLE_EQ(view.rows()[0].cpuPercent.value_or(-1.0), 50.0);
    EXPECT_FALSE(view.rows()[1].cpuPercent.has_value());
    EXPECT_FALSE(view.rows()[2].cpuPercent.has_value());
}

// #3069: another user's process: a refused read offers "Authenticate to read" where the reader can read as an
// administrator, and the press makes the next update read that way.
TEST(ProcessThreadsViewTest, ARefusedReadOffersAuthenticateToReadAndThePressReadsAsAdministrator)
{
    ProcessThreadsView view(1);
    MockThreadsReader reader;
    reader.result = {.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}};
    reader.authorizedResult = threads({thread(100, "main", 1)});

    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F, 1000, 0.0));
    view.finishPendingRead(TARGET, 0.0);
    EXPECT_FALSE(view.offersAuthenticateToRead()) << "this reader can't read as an administrator";

    reader.canAuthorize = true;
    view.markDrawnOpen();
    static_cast<void>(view.update(&reader, TARGET, 0.0F, 1000, 0.1)); // records that it can
    view.finishPendingRead(TARGET, 0.1);
    ASSERT_TRUE(view.offersAuthenticateToRead());

    view.requestAuthorizedRead();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F, 1000, 0.2));
    view.finishPendingRead(TARGET, 0.2);
    EXPECT_EQ(reader.authorizedReads.load(), 1);
    EXPECT_EQ(view.status(), ThreadsReadStatus::Ok);
    ASSERT_EQ(view.rows().size(), 1U);
    EXPECT_FALSE(view.offersAuthenticateToRead());
}

TEST(ProcessThreadsViewTest, ReadsOnlyWhileOpenAtTheSettingsInterval)
{
    ProcessThreadsView view(1);
    MockThreadsReader reader;
    reader.result = threads({thread(100, "main", 1)});

    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F, 1000, 0.0)); // never drawn open: never read
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F, 1000, 0.0));
    view.finishPendingRead(TARGET, 0.0);
    EXPECT_TRUE(view.hasRead());
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 0.5F, 1000, 0.5)); // not yet due at a 1 s interval
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.6F, 1000, 1.1));
    view.finishPendingRead(TARGET, 1.1);
    // A 5 s interval chosen in Settings: the next read waits for it.
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 2.0F, 5000, 3.1));
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 3.0F, 5000, 6.1));
    view.finishPendingRead(TARGET, 6.1);
    EXPECT_EQ(reader.reads.load(), 3);

    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_TRUE(view.rows().empty());
}

TEST(ProcessThreadsViewTest, SortAndFilterByNameOrTid)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(300, "gmain", 1), thread(100, "server", 2), thread(205, "pool-2", 3)}), 0.0);
    view.setSort(Detail::ThreadsColumn::Tid, true);
    ASSERT_EQ(view.rows().size(), 3U);
    EXPECT_EQ(view.rows()[0].thread.tid, 100);
    EXPECT_EQ(view.rows()[2].thread.tid, 300);
    view.setSort(Detail::ThreadsColumn::Name, true);
    EXPECT_EQ(view.rows()[0].thread.name, "gmain");

    view.setFilter("POOL");
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_EQ(view.rows()[view.filteredRows()[0]].thread.tid, 205);
    view.setFilter("30");
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_EQ(view.rows()[view.filteredRows()[0]].thread.tid, 300);
}

// #3069: each thread's start as a time, from the boot time the read gave, sortable; the dash without one.
TEST(ProcessThreadsViewTest, StartedIsTheBootTimePlusTheThreadsStartAndSorts)
{
    ProcessThreadsView view(1);
    ThreadsReadResult read = threads({thread(100, "server", 1, 500), thread(105, "pool", 2, 300)});
    read.ticksPerSecond = 100;
    read.bootTimeEpoch = 1'761'900'000;
    view.applyResult(read, 0.0);
    view.setSort(Detail::ThreadsColumn::Started, true);
    ASSERT_EQ(view.rows().size(), 2U);
    EXPECT_EQ(view.rows()[0].thread.tid, 105) << "started first";
    EXPECT_EQ(view.rows()[0].started, UI::Format::formatEpochDateTimeShort(1'761'900'003));
    EXPECT_EQ(view.rows()[1].started, UI::Format::formatEpochDateTimeShort(1'761'900'005));

    read.bootTimeEpoch = 0;
    view.applyResult(read, 1.0);
    EXPECT_EQ(view.rows()[0].started, std::string(Detail::THREAD_CPU_UNKNOWN));
    EXPECT_EQ(Detail::threadStartedText(100, 0, 1000), UI::Format::formatEpochDateTimeShort(1100)) << "a zero tick rate counts as one";
}

TEST(ProcessThreadsViewTest, AFailedReadShowsItsStatusAndForgetsTheCpuTimes)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(100, "main", 1)}), 0.0);
    view.applyResult({.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}}, 1.0);
    EXPECT_EQ(view.status(), ThreadsReadStatus::PermissionDenied);
    EXPECT_TRUE(view.rows().empty());
    EXPECT_FALSE(Detail::threadsStatusText(ThreadsReadStatus::PermissionDenied).empty());
    EXPECT_TRUE(Detail::threadsStatusText(ThreadsReadStatus::Ok).empty());
    // The next good read starts CPU % afresh.
    view.applyResult(threads({thread(100, "main", 50)}), 2.0);
    EXPECT_FALSE(view.rows()[0].cpuPercent.has_value());
}

TEST(ProcessThreadsViewTest, EachUnsuccessfulReadHasItsOwnStatusLine)
{
    EXPECT_EQ(Detail::threadsStatusText(ThreadsReadStatus::PermissionDenied), "Access denied: another user's process");
    EXPECT_EQ(Detail::threadsStatusText(ThreadsReadStatus::ProcessExited), "Process exited");
    EXPECT_EQ(Detail::threadsStatusText(ThreadsReadStatus::Unsupported), "Not available on this platform");
    EXPECT_EQ(Detail::threadsStatusText(ThreadsReadStatus::IdentityUnknown), "Not available for this process");
    EXPECT_EQ(Detail::threadsStatusText(ThreadsReadStatus::Failed), "Could not be read");
}

TEST(ProcessThreadsViewTest, CpuPercentNeedsCpusAndTimeGoingForward)
{
    EXPECT_FALSE(Detail::threadCpuPercent(5, 100, 1.0, 0).has_value());
    EXPECT_FALSE(Detail::threadCpuPercent(5, 100, -1.0, 1).has_value());
    EXPECT_FALSE(Detail::threadCpuPercent(5, -100, 1.0, 1).has_value());
}

TEST(ProcessThreadsViewTest, AThreadWhoseTicksWentBackwardsHasNoCpuPercent)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(100, "main", 500)}), 0.0);
    // Fewer ticks than last time for the same thread (same start): nothing to subtract from.
    view.applyResult(threads({thread(100, "main", 400)}), 1.0);
    ASSERT_EQ(view.rows().size(), 1U);
    EXPECT_FALSE(view.rows()[0].cpuPercent.has_value());
    EXPECT_EQ(view.rows()[0].cpu, Detail::THREAD_CPU_UNKNOWN);
}

TEST(ProcessThreadsViewTest, SortsByStateCpuTimeAndNice)
{
    ProcessThread running = thread(100, "a", 300);
    running.state = 'R';
    running.nice = 5;
    ProcessThread sleeping = thread(101, "b", 100);
    sleeping.nice = -5;
    ProcessThread idle = thread(102, "c", 200);
    idle.nice = 0;

    ProcessThreadsView view(1);
    view.applyResult(threads({running, sleeping, idle}), 0.0);
    ASSERT_EQ(view.rows().size(), 3U);

    view.setSort(Detail::ThreadsColumn::State, true);
    EXPECT_EQ(view.rows()[0].state, "Running");
    EXPECT_EQ(view.rows()[0].thread.tid, 100);
    EXPECT_EQ(view.rows()[1].thread.tid, 101) << "equal states keep TID order";
    EXPECT_EQ(view.rows()[2].thread.tid, 102);

    view.setSort(Detail::ThreadsColumn::CpuTime, true);
    EXPECT_EQ(view.rows()[0].thread.tid, 101); // 1 s
    EXPECT_EQ(view.rows()[1].thread.tid, 102); // 2 s
    EXPECT_EQ(view.rows()[2].thread.tid, 100); // 3 s
    view.setSort(Detail::ThreadsColumn::CpuTime, false);
    EXPECT_EQ(view.rows()[0].thread.tid, 100);

    view.setSort(Detail::ThreadsColumn::Nice, true);
    EXPECT_EQ(view.rows()[0].thread.tid, 101); // -5
    EXPECT_EQ(view.rows()[1].thread.tid, 102); // 0
    EXPECT_EQ(view.rows()[2].thread.tid, 100); // 5
}

// #3069 (Windows): the STATE cell names a suspended thread's suspend count, else a waiting one's wait reason.
TEST(ProcessThreadsViewTest, TheStateCellSaysTheSuspendCountOrTheWaitReason)
{
    ProcessThread suspended = thread(1, "a", 0);
    suspended.state = 'T';
    suspended.waitReason = "Suspended";
    suspended.suspendCount = 2;
    ProcessThread waiting = thread(2, "b", 0);
    waiting.waitReason = "UserRequest";
    ProcessThread running = thread(3, "c", 0);
    running.state = 'R';
    EXPECT_EQ(Detail::threadStateText(suspended), "Stopped (suspend count 2)");
    EXPECT_EQ(Detail::threadStateText(waiting), "Sleeping (UserRequest)");
    EXPECT_EQ(Detail::threadStateText(running), "Running");
    EXPECT_TRUE(Detail::threadCanResume(suspended));
    EXPECT_FALSE(Detail::threadCanResume(waiting));
    ProcessThread stoppedUncounted = running;
    stoppedUncounted.state = 'T';
    EXPECT_TRUE(Detail::threadCanResume(stoppedUncounted)) << "suspended, its count unreadable";

    ProcessThreadsView view(1);
    view.applyResult(threads({suspended}), 0.0);
    EXPECT_EQ(view.rows()[0].state, "Stopped (suspend count 2)");
}

// #3069: each confirm names the thread and its process, and End's warns that it can corrupt the process.
TEST(ThreadActionControlTest, EachConfirmNamesTheThreadAndEndWarnsOfCorruption)
{
    PendingThreadAction request{
        .target = TARGET, .tid = 105, .threadName = "worker", .processName = "server", .action = Platform::ThreadAction::End};
    const std::string end = threadActionQuestion(request);
    EXPECT_TRUE(end.starts_with("End thread 105 (worker) of server (PID 100)?")) << end;
    EXPECT_TRUE(end.contains("can corrupt the process")) << end;
    EXPECT_TRUE(end.contains("can't be undone")) << end;
    request.action = Platform::ThreadAction::Suspend;
    request.threadName.clear();
    EXPECT_TRUE(threadActionQuestion(request).starts_with("Suspend thread 105 of server (PID 100)?"));
    EXPECT_TRUE(threadActionQuestion(request).contains("may hang"));
    request.action = Platform::ThreadAction::Resume;
    EXPECT_TRUE(threadActionQuestion(request).starts_with("Resume thread 105 of server (PID 100)?"));

    EXPECT_STREQ(threadActionTitle(Platform::ThreadAction::Suspend), "Suspend thread");
    EXPECT_STREQ(threadActionTitle(Platform::ThreadAction::Resume), "Resume thread");
    EXPECT_STREQ(threadActionTitle(Platform::ThreadAction::End), "End thread");
    EXPECT_STREQ(threadActionConfirmLabel(Platform::ThreadAction::Suspend), "Suspend Thread");
    EXPECT_STREQ(threadActionConfirmLabel(Platform::ThreadAction::Resume), "Resume Thread");
    EXPECT_STREQ(threadActionConfirmLabel(Platform::ThreadAction::End), "End Thread");
    EXPECT_TRUE(threadActionIsDestructive(Platform::ThreadAction::End));
    EXPECT_FALSE(threadActionIsDestructive(Platform::ThreadAction::Suspend));
}

[[nodiscard]] Platform::ProcessActionCapabilities threadActionCapabilities(bool canAuthorize = false)
{
    Platform::ProcessActionCapabilities capabilities;
    capabilities.canActOnThreads = true;
    capabilities.canAuthorizeInteractively = canAuthorize;
    return capabilities;
}

TEST(ThreadActionControlTest, AConfirmedActionRunsOnTheWorkerAndSaysWhatItDid)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(threadActionCapabilities());
    actions.setThreadActionResult(Platform::ProcessActionResult::ok("its suspend count is now 2"));
    ProcessThreadsView view(1);
    MockThreadsReader reader;
    ProcessThread worker = thread(105, "worker", 1);
    worker.createTime = 0x01DC'0000'0000'1234ULL;
    reader.result = threads({worker});
    view.markDrawnOpen();
    static_cast<void>(view.update(&reader, TARGET, 0.0F, 1000, 0.0));
    view.finishPendingRead(TARGET, 0.0);
    view.setThreadActions(true, "server");

    view.requestThreadAction(view.rows()[0].thread, Platform::ThreadAction::Suspend);
    ThreadActionControl& control = view.actionControl();
    ASSERT_TRUE(control.pending().has_value());
    EXPECT_TRUE(control.showConfirm());
    EXPECT_EQ(control.pending()->target, TARGET);
    control.confirm(&actions);
    control.update(0.0F, /*wait=*/true);
    EXPECT_EQ(actions.threadActionCount(), 1);
    EXPECT_EQ(actions.lastThreadId(), 105);
    EXPECT_EQ(actions.lastThreadAction(), Platform::ThreadAction::Suspend);
    EXPECT_EQ(actions.lastThreadCreateTime(), worker.createTime) << "the thread confirmed, so a reused TID is refused";
    EXPECT_EQ(actions.lastTarget(), TARGET);
    EXPECT_TRUE(control.result().ok);
    EXPECT_EQ(control.result().text, "Suspended thread 105 (worker) of server (its suspend count is now 2)");
    EXPECT_FALSE(control.pending().has_value());

    // The next update reads the threads again at once, for the new state.
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F, 1000, 0.1)) << "re-read without waiting for the interval";
    view.finishPendingRead(TARGET, 0.1);
    EXPECT_EQ(reader.reads.load(), 2);
}

TEST(ThreadActionControlTest, ARefusalOffersAuthenticateAndRetryWhichAsksTheHelper)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(threadActionCapabilities(/*canAuthorize=*/true));
    actions.setThreadActionResult(Platform::ProcessActionResult::denied("Access denied: can't end thread 7 of process 100"));
    ThreadActionControl control;
    control.request({.target = TARGET,
                     .tid = 7,
                     .threadCreateTime = 70,
                     .threadName = {},
                     .processName = "svc",
                     .action = Platform::ThreadAction::End});
    control.confirm(&actions);
    control.update(0.0F, true);
    EXPECT_FALSE(control.result().ok);
    EXPECT_EQ(control.result().text, "Couldn't end thread 7: Access denied: can't end thread 7 of process 100");
    ASSERT_TRUE(control.offersAuthenticateAndRetry());

    control.retryAuthorized();
    control.update(0.0F, true);
    EXPECT_EQ(actions.runAuthorizedCount(), 1);
    const Platform::AuthorizedAction& sent = actions.lastAuthorizedAction();
    EXPECT_EQ(sent.kind, Platform::AuthorizedAction::Kind::ThreadAction);
    EXPECT_EQ(sent.target, TARGET);
    EXPECT_EQ(sent.tid, 7);
    EXPECT_EQ(sent.threadCreateTime, 70U);
    EXPECT_EQ(sent.threadAction, Platform::ThreadAction::End);
    EXPECT_TRUE(control.result().ok);
    EXPECT_EQ(control.result().text, "Ended thread 7 of svc");
    EXPECT_FALSE(control.offersAuthenticateAndRetry());
    EXPECT_TRUE(control.takeActed());
    EXPECT_FALSE(control.takeActed());
}

TEST(ThreadActionControlTest, NoActionsOrNoCapabilityRefusesAndACancelDoesNothing)
{
    ThreadActionControl control;
    control.request({.target = TARGET, .tid = 7, .threadName = {}, .processName = "svc", .action = Platform::ThreadAction::Suspend});
    control.confirm(nullptr);
    EXPECT_FALSE(control.result().ok);
    EXPECT_EQ(control.result().text, "Couldn't suspend thread 7: Acting on one thread isn't available");

    TestMocks::MockProcessActions noThreadActions; // canActOnThreads false, as on Linux
    control.request({.target = TARGET, .tid = 7, .threadName = {}, .processName = "svc", .action = Platform::ThreadAction::Resume});
    control.cancel();
    EXPECT_FALSE(control.pending().has_value());
    control.confirm(&noThreadActions);
    EXPECT_EQ(noThreadActions.threadActionCount(), 0) << "nothing pending";
    control.request({.target = TARGET, .tid = 7, .threadName = {}, .processName = "svc", .action = Platform::ThreadAction::Resume});
    control.confirm(&noThreadActions);
    EXPECT_EQ(noThreadActions.threadActionCount(), 0);
    EXPECT_FALSE(control.result().ok);

    // The result line goes after a while, and a new selection drops it at once.
    control.update(ThreadActionControl::RESULT_SECONDS + 1.0F);
    EXPECT_TRUE(control.result().empty());
    control.request({.target = TARGET, .tid = 7, .threadName = {}, .processName = "svc", .action = Platform::ThreadAction::Resume});
    control.onSelectionChanged();
    EXPECT_TRUE(control.pending().has_value()) << "left for the confirm to close: its process is no longer shown";
    EXPECT_TRUE(control.result().empty());
}

/// What one frame of @p view draws, its header opened first when @p open.
[[nodiscard]] std::string drawOnce(ProcessThreadsView& view, bool open = true)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::SetNextItemOpen(open);
    ImGui::LogToBuffer();
    view.render(true);
    std::string captured = GImGui->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    return captured;
}

/// What one frame of @p view's thread action confirm draws, for @p liveTarget.
[[nodiscard]] std::string
drawConfirmOnce(ProcessThreadsView& view, Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::LogToBuffer();
    view.renderConfirmation(actions, liveTarget);
    std::string captured = GImGui->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    return captured;
}

class ProcessThreadsViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }
    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessThreadsViewRenderTest, DrawsTheCountAndEachThread)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(100, "server", 1), thread(105, "pool-1", 2)}), 0.0);
    static_cast<void>(drawOnce(view));
    const std::string captured = drawOnce(view);
    EXPECT_TRUE(captured.contains("Threads (2)")) << captured;
    EXPECT_TRUE(captured.contains("server")) << captured;
    EXPECT_TRUE(captured.contains("pool-1")) << captured;
    EXPECT_TRUE(captured.contains("Sleeping")) << captured;
}

// #3069 (Windows): the priority column says PRIORITY for Windows' priorities, and each row's menu offers
// Suspend, Resume (for a suspended thread) and End; Linux's rows have no menu.
TEST_F(ProcessThreadsViewRenderTest, WindowsRowsHaveAMenuAndTheirPriorityColumn)
{
    ProcessThreadsView view(1);
    ThreadsReadResult read = threads({thread(100, "server", 1), thread(105, "pool-1", 2)});
    read.windowsPriorities = true;
    view.applyResult(read, 0.0);
    view.setThreadActions(true, "server");
    static_cast<void>(drawOnce(view));
    std::string captured = drawOnce(view);
    EXPECT_TRUE(captured.contains("PRIORITY")) << captured;
    EXPECT_FALSE(captured.contains("NICE")) << captured;

    view.openRowMenu(105);
    static_cast<void>(drawOnce(view)); // the popup's first frame sizes it, its items clipped
    captured = drawOnce(view);
    EXPECT_TRUE(captured.contains("thread 105 (pool-1)")) << captured;
    EXPECT_TRUE(captured.contains("Suspend thread...")) << captured;
    EXPECT_TRUE(captured.contains("Resume thread...")) << captured;
    EXPECT_TRUE(captured.contains("End thread...")) << captured;
}

TEST_F(ProcessThreadsViewRenderTest, LinuxRowsHaveNoMenu)
{
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(100, "server", 1)}), 0.0);
    view.setThreadActions(false, "server");
    view.openRowMenu(100);
    static_cast<void>(drawOnce(view));
    const std::string captured = drawOnce(view);
    EXPECT_TRUE(captured.contains("NICE")) << captured;
    EXPECT_FALSE(captured.contains("Suspend thread")) << captured;
    EXPECT_FALSE(captured.contains("End thread")) << captured;
}

TEST_F(ProcessThreadsViewRenderTest, TheConfirmAsksItsQuestionAndClosesWhenTheProcessChanges)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(threadActionCapabilities());
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(105, "worker", 1)}), 0.0);
    view.setThreadActions(true, "server");
    static_cast<void>(view.update(nullptr, TARGET, 0.0F, 1000, 0.0)); // the process shown
    view.requestThreadAction(view.rows()[0].thread, Platform::ThreadAction::End);

    static_cast<void>(drawConfirmOnce(view, &actions, TARGET));
    static_cast<void>(drawConfirmOnce(view, &actions, TARGET)); // sized, then drawn
    const std::string captured = drawConfirmOnce(view, &actions, TARGET);
    EXPECT_TRUE(captured.contains("End thread 105 (worker) of server (PID 100)?")) << captured;
    EXPECT_TRUE(captured.contains("can corrupt the process")) << captured;
    EXPECT_TRUE(view.actionControl().pending().has_value());

    // Another process shown: closed unconfirmed, nothing done.
    static_cast<void>(drawConfirmOnce(view, &actions, {.pid = 200, .startTimeTicks = 1}));
    EXPECT_FALSE(view.actionControl().pending().has_value());
    EXPECT_EQ(actions.threadActionCount(), 0);
}

TEST_F(ProcessThreadsViewRenderTest, TheResultLineShowsAboveTheTable)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(threadActionCapabilities());
    actions.setThreadActionResult(Platform::ProcessActionResult::error("Thread 105 not found - it may have already exited"));
    ProcessThreadsView view(1);
    view.applyResult(threads({thread(105, "worker", 1)}), 0.0);
    view.setThreadActions(true, "server");
    view.requestThreadAction(view.rows()[0].thread, Platform::ThreadAction::Resume);
    view.actionControl().confirm(&actions);
    view.actionControl().update(0.0F, true);
    static_cast<void>(drawOnce(view));
    const std::string captured = drawOnce(view);
    EXPECT_TRUE(captured.contains("Couldn't resume thread 105 (worker): Thread 105 not found")) << captured;
}

// #3221: each state the section can be in draws its own line: collapsed nothing, before the first read
// "Reading...", a failed read its status (with the reason a Failed read gives), a cut-short list how many
// it shows, and a filter matching nothing says so rather than drawing an empty table.
TEST_F(ProcessThreadsViewRenderTest, EachStateDrawsItsOwnLine)
{
    ProcessThreadsView view(1);
    EXPECT_FALSE(drawOnce(view, false).contains("Reading...")) << "collapsed: nothing below the header";
    EXPECT_TRUE(drawOnce(view).contains("Reading..."));

    view.applyResult({.status = ThreadsReadStatus::Failed, .threads = {}, .detail = "I/O error"}, 1.0);
    const std::string failed = drawOnce(view);
    EXPECT_TRUE(failed.contains("Could not be read: I/O error")) << failed;

    view.applyResult({.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}}, 2.0);
    const std::string refused = drawOnce(view);
    EXPECT_TRUE(refused.contains("Access denied: another user's process")) << refused;

    ThreadsReadResult cut = threads({thread(100, "server", 1), thread(105, "pool-1", 2)});
    cut.truncated = true;
    view.applyResult(cut, 3.0);
    const std::string truncated = drawOnce(view);
    EXPECT_TRUE(truncated.contains("Showing the first 2 threads")) << truncated;

    view.setFilter("no-such-thread");
    const std::string filtered = drawOnce(view);
    EXPECT_TRUE(filtered.contains("No threads match the filter")) << filtered;
    EXPECT_FALSE(filtered.contains("pool-1")) << filtered;
}

} // namespace
} // namespace App
