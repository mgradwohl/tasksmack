/// @file test_ProcessWaitChainDialog.cpp
/// @brief "Analyze wait chain..." (#3084): which menus offer it, how a chain's nodes are worded, the threads
/// ordered deadlocks first, End process offered only for another process it can name exactly, the summary
/// line; the dialog's read on its worker through a mock IProcessActions (taken in, dropped when closed or
/// replaced, read again), End process confirmed then run, and, headless, the pending state, the chains, a
/// deadlock, the End process button and its confirmation, a failed read and Close.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessWaitChainDialog.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/WaitChain.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ImHashData(), ActivateItemByID()

#include <cstddef>
#include <cstdint>
#include <future>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using WaitChainDetail::ThreadRows;

constexpr Platform::ProcessTarget TARGET{.pid = 7, .startTimeTicks = 700};
constexpr std::int32_t OWN_PID = 1;
constexpr std::int32_t SERVER_PID = 77;
constexpr std::uint64_t SERVER_START = 7700;

[[nodiscard]] Platform::WaitChainNode thread(std::int32_t pid, std::int32_t tid, Platform::WaitNodeStatus status, std::string process = {})
{
    Platform::WaitChainNode node;
    node.type = Platform::WaitObjectType::Thread;
    node.rawType = 8;
    node.status = status;
    node.pid = pid;
    node.tid = tid;
    node.waitTime = 1500;
    node.contextSwitches = 42;
    node.processName = std::move(process);
    node.processStartTimeTicks = (pid == SERVER_PID) ? SERVER_START : 0;
    return node;
}

[[nodiscard]] Platform::WaitChainNode object(Platform::WaitObjectType type, Platform::WaitNodeStatus status, std::string name = {})
{
    Platform::WaitChainNode node;
    node.type = type;
    node.status = status;
    node.objectName = std::move(name);
    return node;
}

[[nodiscard]] Platform::ThreadWaitChain chain(std::int32_t tid, std::vector<Platform::WaitChainNode> nodes, bool isCycle = false)
{
    Platform::ThreadWaitChain result;
    result.tid = tid;
    result.nodes = std::move(nodes);
    result.isCycle = isCycle;
    return result;
}

/// A process with a running thread (10), one waiting on a mutex server.exe holds (11), and two deadlocked
/// on each other's critical sections (12, 13).
[[nodiscard]] Platform::WaitChainReadResult hungRead()
{
    Platform::WaitChainReadResult result;
    result.status = Platform::WaitChainReadStatus::Ok;
    result.threadCount = 4;
    result.threads.push_back(chain(10, {thread(TARGET.pid, 10, Platform::WaitNodeStatus::Running)}));
    result.threads.push_back(chain(11,
                                   {thread(TARGET.pid, 11, Platform::WaitNodeStatus::Blocked),
                                    object(Platform::WaitObjectType::Mutex, Platform::WaitNodeStatus::Owned, "Lock"),
                                    thread(SERVER_PID, 30, Platform::WaitNodeStatus::Blocked, "server.exe")}));
    result.threads.push_back(chain(12,
                                   {thread(TARGET.pid, 12, Platform::WaitNodeStatus::Blocked),
                                    object(Platform::WaitObjectType::CriticalSection, Platform::WaitNodeStatus::Owned),
                                    thread(TARGET.pid, 13, Platform::WaitNodeStatus::Blocked),
                                    object(Platform::WaitObjectType::CriticalSection, Platform::WaitNodeStatus::Owned),
                                    thread(TARGET.pid, 12, Platform::WaitNodeStatus::Blocked)},
                                   true));
    return result;
}

[[nodiscard]] Platform::ProcessActionCapabilities waitChainCapable(bool canKill = true)
{
    Platform::ProcessActionCapabilities caps;
    caps.canKill = canKill;
    caps.canTerminate = true;
    caps.canAnalyzeWaitChain = true;
    return caps;
}

// --- Which menus offer it, and the default ---------------------------------------------------------------

TEST(ProcessWaitChainDialogTest, OfferedForOneProcessWhereThePlatformCanReadOne)
{
    EXPECT_TRUE(WaitChainDetail::offersAnalyzeWaitChain(waitChainCapable(), 0));
    EXPECT_FALSE(WaitChainDetail::offersAnalyzeWaitChain(waitChainCapable(), 2)) << "never for a selection";
    EXPECT_FALSE(WaitChainDetail::offersAnalyzeWaitChain(Platform::ProcessActionCapabilities{}, 0)) << "Linux, the synthetic run";
}

TEST(ProcessWaitChainDialogTest, TheDefaultInterfaceSaysItCantReadOne)
{
    struct Plain : TestMocks::MockProcessActions
    {
        [[nodiscard]] Platform::WaitChainReadResult readWaitChains(const Platform::ProcessTarget& target,
                                                                   const std::stop_token& stopToken) override
        {
            return Platform::IProcessActions::readWaitChains(target, stopToken); // NOLINT(bugprone-parent-virtual-call)
        }
    };
    Plain plain;
    const Platform::WaitChainReadResult result = plain.readWaitChains(TARGET, std::stop_token{});
    EXPECT_EQ(result.status, Platform::WaitChainReadStatus::Unsupported);
    EXPECT_FALSE(plain.actionCapabilities().canAnalyzeWaitChain);
}

// --- Wording -----------------------------------------------------------------------------------------------

TEST(ProcessWaitChainDialogTest, EachNodeSaysWhatItIsAndWhoseItIs)
{
    using WaitChainDetail::nodeText;
    EXPECT_EQ(nodeText(thread(TARGET.pid, 13, Platform::WaitNodeStatus::Blocked), TARGET.pid), "Thread 13 (Blocked)");
    EXPECT_EQ(nodeText(thread(SERVER_PID, 30, Platform::WaitNodeStatus::Running, "server.exe"), TARGET.pid),
              "Thread 30 of server.exe (PID 77) (Running)");
    EXPECT_EQ(nodeText(thread(SERVER_PID, 30, Platform::WaitNodeStatus::NoAccess), TARGET.pid), "Thread 30 of PID 77 (Access denied)");
    EXPECT_EQ(nodeText(thread(SERVER_PID, 0, Platform::WaitNodeStatus::PidOnly, "server.exe"), TARGET.pid),
              "Process server.exe (PID 77) (Thread not known)");
    EXPECT_EQ(nodeText(object(Platform::WaitObjectType::Mutex, Platform::WaitNodeStatus::Abandoned, "Lock"), TARGET.pid),
              "Mutex \"Lock\" (Abandoned)");
    EXPECT_EQ(nodeText(object(Platform::WaitObjectType::Alpc, Platform::WaitNodeStatus::Unknown), TARGET.pid), "ALPC call");
    Platform::WaitChainNode future = object(Platform::WaitObjectType::Unknown, Platform::WaitNodeStatus::Unknown);
    future.rawType = 13;
    EXPECT_EQ(nodeText(future, TARGET.pid), "Unknown (13)");
}

TEST(ProcessWaitChainDialogTest, EveryTypeStatusAndReadStatusHasItsWords)
{
    for (const auto type : {Platform::WaitObjectType::CriticalSection,
                            Platform::WaitObjectType::SendMessageCall,
                            Platform::WaitObjectType::Mutex,
                            Platform::WaitObjectType::Alpc,
                            Platform::WaitObjectType::Com,
                            Platform::WaitObjectType::ThreadWait,
                            Platform::WaitObjectType::ProcessWait,
                            Platform::WaitObjectType::Thread,
                            Platform::WaitObjectType::ComActivation,
                            Platform::WaitObjectType::SocketIo,
                            Platform::WaitObjectType::SmbIo})
    {
        const std::string label = WaitChainDetail::objectTypeLabel(type, 0);
        EXPECT_FALSE(label.empty());
        EXPECT_FALSE(label.starts_with("Unknown")) << label;
    }
    for (const auto status : {Platform::WaitNodeStatus::NoAccess,
                              Platform::WaitNodeStatus::Running,
                              Platform::WaitNodeStatus::Blocked,
                              Platform::WaitNodeStatus::PidOnly,
                              Platform::WaitNodeStatus::PidOnlyRpcss,
                              Platform::WaitNodeStatus::Owned,
                              Platform::WaitNodeStatus::NotOwned,
                              Platform::WaitNodeStatus::Abandoned,
                              Platform::WaitNodeStatus::Error})
    {
        EXPECT_FALSE(WaitChainDetail::nodeStatusLabel(status).empty());
    }
    EXPECT_TRUE(WaitChainDetail::nodeStatusLabel(Platform::WaitNodeStatus::Unknown).empty());
    EXPECT_TRUE(WaitChainDetail::readStatusText(Platform::WaitChainReadStatus::Ok).empty());
    for (const auto status : {Platform::WaitChainReadStatus::PermissionDenied,
                              Platform::WaitChainReadStatus::ProcessExited,
                              Platform::WaitChainReadStatus::Unsupported,
                              Platform::WaitChainReadStatus::IdentityUnknown,
                              Platform::WaitChainReadStatus::Cancelled,
                              Platform::WaitChainReadStatus::Failed})
    {
        EXPECT_FALSE(WaitChainDetail::readStatusText(status).empty());
    }
}

// --- The tree's rows -----------------------------------------------------------------------------------------

TEST(ProcessWaitChainDialogTest, DeadlocksComeFirstThenWaitingThreadsThenTheRest)
{
    const std::vector<ThreadRows> rows = WaitChainDetail::buildThreadRows(hungRead(), TARGET, OWN_PID, true);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_TRUE(rows[0].deadlocked);
    EXPECT_EQ(rows[0].header, ICON_FA_TRIANGLE_EXCLAMATION " Thread 12: deadlocked");
    EXPECT_EQ(rows[0].nodes.size(), 4U);
    EXPECT_TRUE(rows[1].waiting);
    EXPECT_EQ(rows[1].header, "Thread 11: waiting on Mutex");
    EXPECT_EQ(rows[2].header, "Thread 10: Running");
    EXPECT_TRUE(rows[2].nodes.empty());
    EXPECT_EQ(rows[2].tooltip, "Context switches: 42\nWait time (as Windows reports it): 1500");
}

TEST(ProcessWaitChainDialogTest, EndProcessIsOfferedOnlyForAnotherProcessItCanNameExactly)
{
    const std::vector<ThreadRows> rows = WaitChainDetail::buildThreadRows(hungRead(), TARGET, OWN_PID, true);
    ASSERT_EQ(rows[1].nodes.size(), 2U);
    EXPECT_EQ(rows[1].nodes[0].endTarget.pid, -1) << "an object";
    const WaitChainDetail::NodeRow& owner = rows[1].nodes[1];
    EXPECT_EQ(owner.endTarget, (Platform::ProcessTarget{.pid = SERVER_PID, .startTimeTicks = SERVER_START}));
    EXPECT_EQ(owner.endName, "server.exe");
    for (const WaitChainDetail::NodeRow& node : rows[0].nodes)
    {
        EXPECT_EQ(node.endTarget.pid, -1) << "the analyzed process's own threads: " << node.text;
    }

    // Not where the platform can't end one, not TaskSmack itself, not a process whose start time is unknown.
    EXPECT_EQ(WaitChainDetail::buildThreadRows(hungRead(), TARGET, OWN_PID, false)[1].nodes[1].endTarget.pid, -1);
    EXPECT_EQ(WaitChainDetail::buildThreadRows(hungRead(), TARGET, SERVER_PID, true)[1].nodes[1].endTarget.pid, -1);
    Platform::WaitChainReadResult unknownStart = hungRead();
    unknownStart.threads[1].nodes[2].processStartTimeTicks = 0;
    unknownStart.threads[1].nodes[2].processName.clear();
    const auto rowsUnknown = WaitChainDetail::buildThreadRows(unknownStart, TARGET, OWN_PID, true);
    EXPECT_EQ(rowsUnknown[1].nodes[1].endTarget.pid, -1);
}

TEST(ProcessWaitChainDialogTest, AnOwnerWithoutANameIsEndedByItsPid)
{
    Platform::WaitChainReadResult read = hungRead();
    read.threads[1].nodes[2].processName.clear();
    const auto rows = WaitChainDetail::buildThreadRows(read, TARGET, OWN_PID, true);
    EXPECT_EQ(rows[1].nodes[1].endName, "PID 77");
}

TEST(ProcessWaitChainDialogTest, AChainThatFailedOrWasCutSaysWhy)
{
    Platform::WaitChainReadResult read;
    read.status = Platform::WaitChainReadStatus::Ok;
    Platform::ThreadWaitChain failed;
    failed.tid = 5;
    failed.error = "Access denied";
    read.threads.push_back(failed);
    Platform::ThreadWaitChain cut = chain(6,
                                          {thread(TARGET.pid, 6, Platform::WaitNodeStatus::Blocked),
                                           object(Platform::WaitObjectType::ThreadWait, Platform::WaitNodeStatus::Blocked)});
    cut.truncated = true;
    read.threads.push_back(cut);
    read.threads.push_back(Platform::ThreadWaitChain{.nodes = {}, .error = {}, .tid = 9, .isCycle = false, .truncated = false});
    const auto rows = WaitChainDetail::buildThreadRows(read, TARGET, OWN_PID, true);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].header, "Thread 6: waiting on Wait for a thread"); // waiting sorts first
    EXPECT_EQ(rows[0].note, "The chain goes on past the 16 nodes Windows follows.");
    EXPECT_EQ(rows[1].header, "Thread 5: not read");
    EXPECT_TRUE(rows[1].failed);
    EXPECT_EQ(rows[1].note, "Access denied");
    EXPECT_EQ(rows[2].header, "Thread 9") << "no nodes: nothing more to say";
}

TEST(ProcessWaitChainDialogTest, TheSummaryCountsWaitingAndDeadlockedThreadsAndTheOnesLeftOut)
{
    Platform::WaitChainReadResult read = hungRead();
    auto rows = WaitChainDetail::buildThreadRows(read, TARGET, OWN_PID, true);
    EXPECT_EQ(WaitChainDetail::summaryText(read, rows), "3 threads: 1 waiting on another thread or object, 1 deadlocked");

    read.threadCount = 40;
    read.threadsTruncated = true;
    EXPECT_EQ(WaitChainDetail::summaryText(read, rows),
              "3 threads: 1 waiting on another thread or object, 1 deadlocked. 37 more not read: the process has too many threads, or "
              "they took too long");

    Platform::WaitChainReadResult idle;
    idle.status = Platform::WaitChainReadStatus::Ok;
    idle.threads.push_back(chain(10, {thread(TARGET.pid, 10, Platform::WaitNodeStatus::Running)}));
    rows = WaitChainDetail::buildThreadRows(idle, TARGET, OWN_PID, true);
    EXPECT_EQ(WaitChainDetail::summaryText(idle, rows), "1 thread, none waiting on another thread or object");
}

// --- The dialog's state ------------------------------------------------------------------------------------

TEST(ProcessWaitChainDialogTest, OpeningReadsOnTheWorkerAndTakesTheChainsIn)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(waitChainCapable());
    actions.setWaitChainResult(hungRead());
    actions.holdWaitChains();
    ProcessWaitChainDialog dialog;
    dialog.open(&actions, TARGET, "app.exe", OWN_PID);
    EXPECT_TRUE(dialog.isOpen());
    EXPECT_TRUE(dialog.reading());
    EXPECT_FALSE(dialog.result().has_value());
    EXPECT_EQ(dialog.target(), TARGET);
    EXPECT_EQ(dialog.endAction(), Detail::ProcessAction::Kill);

    actions.releaseWaitChains();
    dialog.update(true);
    EXPECT_FALSE(dialog.reading());
    ASSERT_TRUE(dialog.result().has_value());
    EXPECT_EQ(dialog.result()->status, Platform::WaitChainReadStatus::Ok);
    EXPECT_EQ(dialog.threads().size(), 3U);
    EXPECT_EQ(dialog.summary(), "3 threads: 1 waiting on another thread or object, 1 deadlocked");
    EXPECT_EQ(actions.waitChainCount(), 1);
    EXPECT_EQ(actions.lastTarget(), TARGET);

    dialog.refresh();
    dialog.update(true);
    EXPECT_EQ(actions.waitChainCount(), 2);
    dialog.shutdown();
    EXPECT_FALSE(dialog.isOpen());
}

TEST(ProcessWaitChainDialogTest, WithoutActionsItSaysItCant)
{
    ProcessWaitChainDialog dialog;
    dialog.open(nullptr, TARGET, "app.exe", OWN_PID);
    EXPECT_FALSE(dialog.reading());
    ASSERT_TRUE(dialog.result().has_value());
    EXPECT_EQ(dialog.result()->status, Platform::WaitChainReadStatus::Failed);
    EXPECT_EQ(dialog.result()->detail, "Process actions unavailable");
    EXPECT_EQ(dialog.endAction(), Detail::ProcessAction::None);
}

TEST(ProcessWaitChainDialogTest, ClosingDropsTheReadInFlight)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(waitChainCapable());
    actions.setWaitChainResult(hungRead());
    actions.holdWaitChains();
    ProcessWaitChainDialog dialog;
    dialog.open(&actions, TARGET, "app.exe", OWN_PID);
    dialog.close();
    EXPECT_FALSE(dialog.isOpen());
    dialog.update(true); // the held read was asked to stop: it ends Cancelled, and is dropped
    EXPECT_FALSE(dialog.result().has_value());
    dialog.refresh();
    EXPECT_FALSE(dialog.reading()) << "a closed dialog reads nothing";
}

// Opened for another process while the first read still runs: that read is stopped and dropped, and the new
// process's read starts once it has ended.
TEST(ProcessWaitChainDialogTest, OpeningForAnotherProcessReplacesTheReadInFlight)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(waitChainCapable(/*canKill=*/false));
    actions.setWaitChainResult(hungRead());
    actions.holdWaitChains();
    ProcessWaitChainDialog dialog;
    dialog.open(&actions, TARGET, "app.exe", OWN_PID);
    constexpr Platform::ProcessTarget OTHER{.pid = 8, .startTimeTicks = 800};
    dialog.open(&actions, OTHER, "other.exe", OWN_PID);
    EXPECT_TRUE(dialog.reading());
    EXPECT_EQ(dialog.endAction(), Detail::ProcessAction::Terminate) << "Terminate where the platform can't kill";

    dialog.update(true); // the first read ends Cancelled and is dropped; the second starts, and is held
    EXPECT_FALSE(dialog.result().has_value());
    actions.releaseWaitChains();
    dialog.update(true);
    ASSERT_TRUE(dialog.result().has_value());
    EXPECT_EQ(dialog.result()->status, Platform::WaitChainReadStatus::Ok);
    EXPECT_EQ(actions.lastTarget(), OTHER);
    EXPECT_EQ(actions.waitChainCount(), 2);
}

TEST(ProcessWaitChainDialogTest, EndProcessIsConfirmedThenKillsAndReadsAgain)
{
    TestMocks::MockProcessActions actions;
    actions.setCapabilities(waitChainCapable());
    actions.setWaitChainResult(hungRead());
    ProcessWaitChainDialog dialog;
    dialog.open(&actions, TARGET, "app.exe", OWN_PID);
    dialog.update(true);

    constexpr Platform::ProcessTarget SERVER{.pid = SERVER_PID, .startTimeTicks = SERVER_START};
    dialog.requestEnd(SERVER, "server.exe");
    EXPECT_EQ(dialog.endTarget(), SERVER);
    EXPECT_EQ(actions.killCount(), 0) << "asked, not done";

    dialog.confirmEnd();
    EXPECT_EQ(dialog.endTarget().pid, -1);
    dialog.update(true); // the kill, then the read it starts
    dialog.update(true);
    EXPECT_EQ(actions.killCount(), 1);
    EXPECT_EQ(actions.lastKillPid(), SERVER_PID);
    EXPECT_TRUE(dialog.endResult().ok);
    EXPECT_EQ(dialog.endResult().text, "Kill sent to PID 77");
    EXPECT_EQ(actions.waitChainCount(), 2) << "read again once it ended";

    // A refusal says so, and reads nothing again.
    actions.setKillResult(Platform::ProcessActionResult::error("Access denied"));
    dialog.requestEnd(SERVER, "server.exe");
    dialog.confirmEnd();
    dialog.update(true);
    EXPECT_FALSE(dialog.endResult().ok);
    EXPECT_EQ(actions.waitChainCount(), 2);
    dialog.confirmEnd(); // nothing pending: nothing happens
    EXPECT_EQ(actions.killCount(), 2);
}

TEST(ProcessWaitChainDialogTest, EndProcessIsntOfferedWhereThePlatformCantEndOne)
{
    TestMocks::MockProcessActions actions;
    Platform::ProcessActionCapabilities caps;
    caps.canAnalyzeWaitChain = true;
    actions.setCapabilities(caps);
    actions.setWaitChainResult(hungRead());
    ProcessWaitChainDialog dialog;
    dialog.open(&actions, TARGET, "app.exe", OWN_PID);
    dialog.update(true);
    EXPECT_EQ(dialog.endAction(), Detail::ProcessAction::None);
    EXPECT_EQ(dialog.threads()[1].nodes[1].endTarget.pid, -1);
    dialog.requestEnd({.pid = SERVER_PID, .startTimeTicks = SERVER_START}, "server.exe");
    EXPECT_EQ(dialog.endTarget().pid, -1);
}

// --- Headless ------------------------------------------------------------------------------------------------

class ProcessWaitChainDialogRenderTest : public ::testing::Test
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
        m_Actions.setCapabilities(waitChainCapable());
        m_Actions.setWaitChainResult(hungRead());
    }

    void TearDown() override
    {
        m_Dialog.shutdown();
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window standing in for the panel, drawing the dialog; the text it drew.
    std::string frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        m_Dialog.render();
        std::string text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    }

    /// The text of the next few frames, joined: a modal opened this frame draws its contents on the next.
    std::string frames(int count = 3)
    {
        std::string text;
        for (int i = 0; i < count; ++i)
        {
            text += frame();
        }
        return text;
    }

    /// Presses the End process button of the tree's row @p row (its thread's index in threads()) at depth
    /// @p depth (1: the node after the thread), on the next frame: its ID, as the tree builds it.
    static void pressEndProcess(int row, int depth)
    {
        const ImGuiWindow* tree = nullptr;
        for (const ImGuiWindow* window : GImGui->Windows)
        {
            if (std::string_view(window->Name).contains("##WaitChains"))
            {
                tree = window;
            }
        }
        ASSERT_NE(tree, nullptr);
        ImGuiID id = ImHashData(&row, sizeof(row), tree->ID);
        id = ImHashStr("##Thread", 0, id);
        for (int level = 0; level < depth; ++level)
        {
            id = ImHashStr("##Node", 0, id);
        }
        ImGui::ActivateItemByID(ImHashStr("End process...", 0, id));
    }

    /// Presses the top-most modal's button @p label (@p filled: the danger-filled confirm) on the next frame.
    static void pressModal(const char* label, bool filled)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        const ImGuiID id = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, id) : id);
    }

    TestMocks::MockProcessActions m_Actions;
    ProcessWaitChainDialog m_Dialog;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessWaitChainDialogRenderTest, SaysItIsReadingThenShowsTheChainsWithTheDeadlockFirst)
{
    m_Actions.holdWaitChains();
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    const std::string pending = frames(2);
    EXPECT_TRUE(pending.contains(WaitChainDetail::READING_TEXT)) << pending;
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).contains("Wait chain of app.exe (PID 7)")) << modal->Name;

    m_Actions.releaseWaitChains();
    m_Dialog.update(true);
    const std::string shown = frames(2);
    EXPECT_FALSE(shown.contains(WaitChainDetail::READING_TEXT));
    EXPECT_TRUE(shown.contains("3 threads: 1 waiting on another thread or object, 1 deadlocked")) << shown;
    EXPECT_TRUE(shown.contains("Thread 12: deadlocked")) << shown;
    EXPECT_TRUE(shown.contains("Critical section (Owned)")) << shown;
    EXPECT_TRUE(shown.contains("Thread 11: waiting on Mutex")) << shown;
    EXPECT_TRUE(shown.contains("Mutex \"Lock\" (Owned)")) << shown;
    EXPECT_TRUE(shown.contains("Thread 30 of server.exe (PID 77) (Blocked)")) << shown;
    EXPECT_TRUE(shown.contains("End process...")) << shown;
    EXPECT_TRUE(shown.contains("Thread 10: Running")) << shown;
    EXPECT_LT(shown.find("Thread 12: deadlocked"), shown.find("Thread 11: waiting")) << "the deadlock first";
}

TEST_F(ProcessWaitChainDialogRenderTest, EndProcessAsksTheSharedConfirmationThenKillsTheOwner)
{
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    static_cast<void>(frames(2));

    pressEndProcess(1, 2); // server.exe's thread: two levels under thread 11
    static_cast<void>(frames(3));
    EXPECT_EQ(m_Dialog.endTarget(), (Platform::ProcessTarget{.pid = SERVER_PID, .startTimeTicks = SERVER_START}));
    const ImGuiWindow* confirm = ImGui::GetTopMostPopupModal();
    ASSERT_NE(confirm, nullptr);
    EXPECT_TRUE(std::string_view(confirm->Name).starts_with("Kill server.exe (PID 77)?")) << confirm->Name;
    EXPECT_EQ(m_Actions.killCount(), 0);

    pressModal("Kill", true);
    static_cast<void>(frames(2));
    m_Dialog.update(true);
    m_Dialog.update(true);
    EXPECT_EQ(m_Actions.killCount(), 1);
    EXPECT_EQ(m_Actions.lastKillPid(), SERVER_PID);
    EXPECT_TRUE(frames(2).contains("Kill sent to PID 77"));
    EXPECT_TRUE(m_Dialog.isOpen()) << "the wait chain dialog stays, with the chains read again";
}

TEST_F(ProcessWaitChainDialogRenderTest, CancellingTheConfirmationEndsNothing)
{
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    static_cast<void>(frames(2));
    pressEndProcess(1, 2);
    static_cast<void>(frames(3));
    pressModal("Cancel", false);
    static_cast<void>(frames(3));
    EXPECT_EQ(m_Dialog.endTarget().pid, -1);
    EXPECT_EQ(m_Actions.killCount(), 0);
    EXPECT_TRUE(m_Dialog.isOpen());
}

TEST_F(ProcessWaitChainDialogRenderTest, AFailedReadSaysWhyAndCloseCloses)
{
    Platform::WaitChainReadResult denied;
    denied.status = Platform::WaitChainReadStatus::Failed;
    denied.detail = "Couldn't open a wait chain session: Not enough memory";
    m_Actions.setWaitChainResult(denied);
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    const std::string shown = frames(2);
    EXPECT_TRUE(shown.contains(WaitChainDetail::readStatusText(Platform::WaitChainReadStatus::Failed))) << shown;
    EXPECT_TRUE(shown.contains("Not enough memory")) << shown;

    pressModal("Close", false);
    static_cast<void>(frames(2));
    EXPECT_FALSE(m_Dialog.isOpen());
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
}

TEST_F(ProcessWaitChainDialogRenderTest, RefreshReadsAgainAndCloseFromOutsideClosesIt)
{
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    static_cast<void>(frames(2));
    pressModal(ICON_FA_ARROWS_ROTATE " Refresh", false);
    static_cast<void>(frames(2));
    m_Dialog.update(true);
    EXPECT_EQ(m_Actions.waitChainCount(), 2);

    m_Dialog.close();
    static_cast<void>(frames(2));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
}

TEST_F(ProcessWaitChainDialogRenderTest, AChainThatCouldntBeReadOrWasCutSaysWhyAndEscapeCloses)
{
    Platform::WaitChainReadResult read;
    read.status = Platform::WaitChainReadStatus::Ok;
    Platform::ThreadWaitChain failed;
    failed.tid = 5;
    failed.error = "The thread has exited";
    read.threads.push_back(failed);
    Platform::ThreadWaitChain cut = chain(6,
                                          {thread(TARGET.pid, 6, Platform::WaitNodeStatus::Blocked),
                                           object(Platform::WaitObjectType::SocketIo, Platform::WaitNodeStatus::Blocked)});
    cut.truncated = true;
    read.threads.push_back(cut);
    m_Actions.setWaitChainResult(read);
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    const std::string shown = frames(2);
    EXPECT_TRUE(shown.contains("Thread 5: not read")) << shown;
    EXPECT_TRUE(shown.contains("The thread has exited")) << shown;
    EXPECT_TRUE(shown.contains("Socket I/O (Blocked)")) << shown;
    EXPECT_TRUE(shown.contains("The chain goes on past the 16 nodes Windows follows.")) << shown;

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    static_cast<void>(frame());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    static_cast<void>(frames(2));
    EXPECT_FALSE(m_Dialog.isOpen()) << "Escape closes it, as Close does";
}

/// Mock actions whose Kill waits until the test lets it finish, so the dialog can be drawn while it runs.
class HeldKillActions : public TestMocks::MockProcessActions
{
  public:
    [[nodiscard]] Platform::ProcessActionResult kill(const Platform::ProcessTarget& target) override
    {
        m_Release.get_future().wait();
        return TestMocks::MockProcessActions::kill(target);
    }
    void release()
    {
        m_Release.set_value();
    }

  private:
    std::promise<void> m_Release;
};

TEST_F(ProcessWaitChainDialogRenderTest, WhileEndProcessRunsTheDialogSaysSo)
{
    HeldKillActions actions;
    actions.setCapabilities(waitChainCapable());
    actions.setWaitChainResult(hungRead());
    m_Dialog.open(&actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    m_Dialog.requestEnd({.pid = SERVER_PID, .startTimeTicks = SERVER_START}, "server.exe");
    m_Dialog.confirmEnd();
    EXPECT_TRUE(m_Dialog.ending());
    const std::string running = frames(2);
    actions.release();
    m_Dialog.update(true);
    EXPECT_TRUE(running.contains("Killing server.exe (PID 77)...")) << running;
    EXPECT_FALSE(m_Dialog.ending());
    EXPECT_EQ(actions.killCount(), 1);
    m_Dialog.shutdown(); // before the actions go
}

TEST_F(ProcessWaitChainDialogRenderTest, AnEmptyReadSaysSo)
{
    Platform::WaitChainReadResult empty;
    empty.status = Platform::WaitChainReadStatus::Ok;
    m_Actions.setWaitChainResult(empty);
    m_Dialog.open(&m_Actions, TARGET, "app.exe", OWN_PID);
    m_Dialog.update(true);
    EXPECT_TRUE(frames(2).contains("No threads were read.")) << "a process with no thread listed";
}

} // namespace
} // namespace App
