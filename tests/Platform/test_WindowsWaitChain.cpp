/// @file test_WindowsWaitChain.cpp
/// @brief Windows' "Analyze wait chain" read (#3084), Platform::Windows::readWaitChains(), against fake
/// advapi32/kernel32 calls: the WCT session opened once and closed once whatever happens after it opens,
/// each thread's chain decoded with its owning processes named and their start times read, and every
/// failure on the way -- an unknown or reused identity, a refused open, a thread listing or session that
/// fails, threads that are refused or have exited, a chain Windows cut short, a stop request, the time
/// budget, COM's callbacks missing. Plus WAITCHAIN_NODE_INFO arrays decoded as Windows lays them out, this
/// process's own threads listed for real, and one real read of a child process this test starts, blocked
/// on a mutex the test holds. GetThreadWaitChain is never called on any other process.

#include "Platform/IProcessActions.h"
#include "Platform/WaitChain.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsWaitChain.h"
#include "Platform/Windows/WindowsWaitChainMath.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wct.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr DWORD TARGET_PID = 4242;
constexpr DWORD OTHER_PID = 777;
constexpr std::uint64_t OTHER_START_TICKS = 0x01DB'0000'9999'0000ULL;
constexpr ProcessTarget TARGET{.pid = static_cast<std::int32_t>(TARGET_PID), .startTimeTicks = START_TICKS};

/// What GetThreadWaitChain answers for one thread.
struct FakeChain
{
    std::vector<WAITCHAIN_NODE_INFO> nodes;
    bool isCycle = false;
    DWORD error = 0; ///< Non-zero: the call fails with it (ERROR_MORE_DATA still fills the array)
};

/// The answers the fakes give, set per test.
struct FakeWct
{
    DWORD openError = 0; ///< Non-zero: OpenProcess fails with it
    std::map<DWORD, std::uint64_t> creationTicks{{TARGET_PID, START_TICKS}, {OTHER_PID, OTHER_START_TICKS}};
    std::optional<ThreadListing> listing;
    bool comRegistered = true;
    DWORD sessionError = 0; ///< Non-zero: OpenThreadWaitChainSession fails with it
    std::map<DWORD, FakeChain> chains;
    std::chrono::milliseconds clockStep{0};
    std::chrono::steady_clock::time_point clock;
    int processOpens = 0;
    int processCloses = 0;
    int sessionOpens = 0;
    int sessionCloses = 0;
    int chainCalls = 0;
    DWORD lastFlags = 0;
};

FakeWct& fake()
{
    static FakeWct instance;
    return instance;
}

/// A fake handle per PID: the address of its slot.
HANDLE handleFor(DWORD pid)
{
    static std::map<DWORD, int> slots;
    return &slots[pid];
}

DWORD pidOf(HANDLE handle)
{
    for (const auto& [pid, ticks] : fake().creationTicks)
    {
        if (handleFor(pid) == handle)
        {
            return pid;
        }
    }
    return 0;
}

HANDLE WINAPI fakeOpenProcess(DWORD /*access*/, BOOL /*inherit*/, DWORD pid)
{
    if (fake().openError != 0 || !fake().creationTicks.contains(pid))
    {
        SetLastError(fake().openError != 0 ? fake().openError : ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    ++fake().processOpens;
    return handleFor(pid);
}

BOOL WINAPI fakeCloseHandle(HANDLE /*handle*/)
{
    ++fake().processCloses;
    return TRUE;
}

BOOL WINAPI fakeGetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    const std::uint64_t ticks = fake().creationTicks.at(pidOf(process));
    creation->dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(ticks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeGetExitCodeProcess(HANDLE /*process*/, LPDWORD code)
{
    *code = STILL_ACTIVE;
    return TRUE;
}

/// The one session the fake hands out.
HWCT fakeSession()
{
    static int token = 0;
    return &token;
}

HWCT WINAPI fakeOpenSession(DWORD flags, PWAITCHAINCALLBACK callback)
{
    EXPECT_EQ(flags, 0U) << "synchronous";
    EXPECT_EQ(callback, nullptr);
    ++fake().sessionOpens;
    if (fake().sessionError != 0)
    {
        SetLastError(fake().sessionError);
        return nullptr;
    }
    return fakeSession();
}

VOID WINAPI fakeCloseSession(HWCT session)
{
    EXPECT_EQ(session, fakeSession());
    ++fake().sessionCloses;
}

BOOL WINAPI fakeGetChain(HWCT session, DWORD_PTR context, DWORD flags, DWORD tid, LPDWORD count, PWAITCHAIN_NODE_INFO nodes, LPBOOL isCycle)
{
    EXPECT_EQ(session, fakeSession());
    EXPECT_EQ(context, 0U);
    EXPECT_EQ(*count, WCT_MAX_NODE_COUNT);
    ++fake().chainCalls;
    fake().lastFlags = flags;
    const auto found = fake().chains.find(tid);
    if (found == fake().chains.end())
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const FakeChain& chain = found->second;
    const std::size_t copied = std::min<std::size_t>(chain.nodes.size(), WCT_MAX_NODE_COUNT);
    std::copy_n(chain.nodes.begin(), copied, nodes);
    *count = static_cast<DWORD>(chain.nodes.size());
    *isCycle = chain.isCycle ? TRUE : FALSE;
    if (chain.error != 0)
    {
        SetLastError(chain.error);
        return FALSE;
    }
    return TRUE;
}

std::optional<ThreadListing> fakeListThreads(DWORD pid)
{
    EXPECT_EQ(pid, TARGET_PID);
    if (!fake().listing.has_value())
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return fake().listing;
}

bool fakeRegisterCom()
{
    return fake().comRegistered;
}

std::chrono::steady_clock::time_point fakeNow()
{
    fake().clock += fake().clockStep;
    return fake().clock;
}

WaitChainFunctions fakeFunctions()
{
    WaitChainFunctions fns;
    fns.identity = {.openProcess = &fakeOpenProcess,
                    .getProcessTimes = &fakeGetProcessTimes,
                    .closeHandle = &fakeCloseHandle,
                    .getExitCodeProcess = &fakeGetExitCodeProcess};
    fns.openSession = &fakeOpenSession;
    fns.closeSession = &fakeCloseSession;
    fns.getChain = &fakeGetChain;
    fns.listThreads = &fakeListThreads;
    fns.registerComCallbacks = &fakeRegisterCom;
    fns.now = &fakeNow;
    return fns;
}

[[nodiscard]] WAITCHAIN_NODE_INFO threadInfo(DWORD pid, DWORD tid, WCT_OBJECT_STATUS status)
{
    WAITCHAIN_NODE_INFO info{};
    info.ObjectType = WctThreadType;
    info.ObjectStatus = status;
    info.ThreadObject.ProcessId = pid;
    info.ThreadObject.ThreadId = tid;
    info.ThreadObject.WaitTime = 1500;
    info.ThreadObject.ContextSwitches = 42;
    return info;
}

[[nodiscard]] WAITCHAIN_NODE_INFO objectInfo(WCT_OBJECT_TYPE type, WCT_OBJECT_STATUS status, std::wstring_view name)
{
    WAITCHAIN_NODE_INFO info{};
    info.ObjectType = type;
    info.ObjectStatus = status;
    std::copy_n(name.begin(), std::min<std::size_t>(name.size(), WCT_OBJNAME_LENGTH), std::begin(info.LockObject.ObjectName));
    return info;
}

class WindowsWaitChainTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeWct{};
        fake().listing = ThreadListing{.threadIds = {10, 20}, .processNames = {{TARGET_PID, "app.exe"}, {OTHER_PID, "server.exe"}}};
        // Thread 10 waits on a mutex a thread of server.exe owns; thread 20 runs.
        fake().chains[10] = {.nodes = {threadInfo(TARGET_PID, 10, WctStatusBlocked),
                                       objectInfo(WctMutexType, WctStatusOwned, L"\\BaseNamedObjects\\Lock"),
                                       threadInfo(OTHER_PID, 30, WctStatusBlocked)},
                             .isCycle = false,
                             .error = 0};
        fake().chains[20] = {.nodes = {threadInfo(TARGET_PID, 20, WctStatusRunning)}, .isCycle = false, .error = 0};
    }

    [[nodiscard]] static WaitChainReadResult read(const ProcessTarget& target = TARGET)
    {
        return readWaitChains(fakeFunctions(), target, std::stop_token{});
    }
};

TEST_F(WindowsWaitChainTest, ReadsEveryThreadsChainAndNamesTheProcessesItLeadsInto)
{
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::Ok);
    EXPECT_EQ(result.threadCount, 2U);
    EXPECT_FALSE(result.threadsTruncated);
    ASSERT_EQ(result.threads.size(), 2U);

    const ThreadWaitChain& waiting = result.threads[0];
    EXPECT_EQ(waiting.tid, 10);
    ASSERT_EQ(waiting.nodes.size(), 3U);
    EXPECT_EQ(waiting.nodes[0].processName, "app.exe");
    EXPECT_EQ(waiting.nodes[0].processStartTimeTicks, START_TICKS) << "the target's own, known already";
    EXPECT_EQ(waiting.nodes[1].type, WaitObjectType::Mutex);
    EXPECT_EQ(waiting.nodes[1].objectName, "\\BaseNamedObjects\\Lock");
    EXPECT_EQ(waiting.nodes[2].pid, static_cast<std::int32_t>(OTHER_PID));
    EXPECT_EQ(waiting.nodes[2].tid, 30);
    EXPECT_EQ(waiting.nodes[2].processName, "server.exe");
    EXPECT_EQ(waiting.nodes[2].processStartTimeTicks, OTHER_START_TICKS);
    EXPECT_EQ(waiting.nodes[2].waitTime, 1500U);
    EXPECT_EQ(waiting.nodes[2].contextSwitches, 42U);

    ASSERT_EQ(result.threads[1].nodes.size(), 1U);
    EXPECT_EQ(result.threads[1].nodes[0].status, WaitNodeStatus::Running);

    EXPECT_EQ(fake().sessionOpens, 1);
    EXPECT_EQ(fake().sessionCloses, 1);
    EXPECT_EQ(fake().chainCalls, 2);
    EXPECT_EQ(fake().lastFlags, static_cast<DWORD>(WCTP_GETINFO_ALL_FLAGS | WCT_NETWORK_IO_FLAG));
    EXPECT_EQ(fake().processOpens, fake().processCloses) << "every process handle closed";
}

TEST_F(WindowsWaitChainTest, AnOwnerProcessThatCantBeOpenedHasNoStartTimeSoNoEndProcess)
{
    fake().creationTicks.erase(OTHER_PID); // another user's, say: OpenProcess refuses it
    const WaitChainReadResult result = read();
    ASSERT_EQ(result.threads.size(), 2U);
    ASSERT_EQ(result.threads[0].nodes.size(), 3U);
    EXPECT_EQ(result.threads[0].nodes[2].processName, "server.exe");
    EXPECT_EQ(result.threads[0].nodes[2].processStartTimeTicks, 0U);
}

TEST_F(WindowsWaitChainTest, AnUnknownOrReusedIdentityOrARefusedOpenReadsNothing)
{
    EXPECT_EQ(read({.pid = TARGET.pid, .startTimeTicks = 0}).status, WaitChainReadStatus::IdentityUnknown);
    EXPECT_EQ(read({.pid = TARGET.pid, .startTimeTicks = START_TICKS + 1}).status, WaitChainReadStatus::ProcessExited);
    fake().openError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(read().status, WaitChainReadStatus::PermissionDenied);
    EXPECT_EQ(fake().sessionOpens, 0) << "no session for a process not confirmed";
    EXPECT_EQ(fake().chainCalls, 0);
}

TEST_F(WindowsWaitChainTest, AThreadListingThatFailsIsAFailureWithNoSession)
{
    fake().listing.reset();
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::Failed);
    EXPECT_TRUE(result.detail.starts_with("Couldn't list the process's threads")) << result.detail;
    EXPECT_EQ(fake().sessionOpens, 0);
    EXPECT_EQ(fake().processOpens, fake().processCloses);
}

TEST_F(WindowsWaitChainTest, ASessionThatWontOpenIsAFailureAndNothingIsClosed)
{
    fake().sessionError = ERROR_NOT_ENOUGH_MEMORY;
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::Failed);
    EXPECT_TRUE(result.detail.starts_with("Couldn't open a wait chain session")) << result.detail;
    EXPECT_EQ(fake().sessionOpens, 1);
    EXPECT_EQ(fake().sessionCloses, 0);
    EXPECT_EQ(fake().chainCalls, 0);
}

TEST_F(WindowsWaitChainTest, EveryThreadRefusedIsAccessDeniedAndTheSessionIsStillClosed)
{
    fake().chains[10].error = ERROR_ACCESS_DENIED;
    fake().chains[20].error = ERROR_ACCESS_DENIED;
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::PermissionDenied);
    ASSERT_EQ(result.threads.size(), 2U);
    EXPECT_EQ(result.threads[0].error, "Access denied");
    EXPECT_TRUE(result.threads[0].nodes.empty());
    EXPECT_EQ(fake().sessionCloses, 1);
}

TEST_F(WindowsWaitChainTest, AThreadThatExitedSaysSoAndTheOthersAreStillRead)
{
    fake().listing->threadIds = {10, 99, 20}; // 99 exited between the listing and its read
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::Ok);
    ASSERT_EQ(result.threads.size(), 3U);
    EXPECT_EQ(result.threads[1].tid, 99);
    EXPECT_EQ(result.threads[1].error, "The thread has exited");
    EXPECT_EQ(result.threads[2].nodes.size(), 1U);
}

TEST_F(WindowsWaitChainTest, AChainWindowsCutShortIsKeptAndMarked)
{
    FakeChain& longChain = fake().chains[10];
    longChain.nodes.clear();
    for (DWORD i = 0; i < WCT_MAX_NODE_COUNT + 4; ++i)
    {
        longChain.nodes.push_back((i % 2 == 0) ? threadInfo(TARGET_PID, 10 + i, WctStatusBlocked)
                                               : objectInfo(WctCriticalSectionType, WctStatusOwned, L""));
    }
    longChain.error = ERROR_MORE_DATA;
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::Ok);
    ASSERT_FALSE(result.threads.empty());
    EXPECT_TRUE(result.threads[0].truncated);
    EXPECT_EQ(result.threads[0].nodes.size(), WCT_MAX_NODE_COUNT);
    EXPECT_TRUE(result.threads[0].error.empty());
}

TEST_F(WindowsWaitChainTest, AStopRequestEndsTheReadAsCancelledAndClosesTheSession)
{
    std::stop_source stop;
    stop.request_stop();
    const WaitChainReadResult result = readWaitChains(fakeFunctions(), TARGET, stop.get_token());
    EXPECT_EQ(result.status, WaitChainReadStatus::Cancelled);
    EXPECT_EQ(fake().chainCalls, 0);
    EXPECT_EQ(fake().sessionCloses, 1);
}

TEST_F(WindowsWaitChainTest, TheTimeBudgetLeavesTheLaterThreadsOutAndSaysSo)
{
    // 3 s a clock read: the first thread is read at 3 s, the second would be at 6 s, past the budget.
    fake().clockStep = std::chrono::milliseconds(3000);
    const WaitChainReadResult result = read();
    EXPECT_EQ(result.status, WaitChainReadStatus::Ok);
    EXPECT_EQ(result.threadCount, 2U);
    EXPECT_EQ(result.threads.size(), 1U);
    EXPECT_TRUE(result.threadsTruncated);
    EXPECT_EQ(fake().sessionCloses, 1);
}

TEST_F(WindowsWaitChainTest, WithoutComsCallbacksComCallsArentAskedFor)
{
    fake().comRegistered = false;
    static_cast<void>(read());
    EXPECT_EQ(fake().lastFlags & WCT_OUT_OF_PROC_COM_FLAG, 0U);
    EXPECT_NE(fake().lastFlags & WCT_OUT_OF_PROC_FLAG, 0U);

    WaitChainFunctions noCom = fakeFunctions();
    noCom.registerComCallbacks = nullptr;
    static_cast<void>(readWaitChains(noCom, TARGET, std::stop_token{}));
    EXPECT_EQ(fake().lastFlags & WCT_OUT_OF_PROC_COM_FLAG, 0U);
}

// WAITCHAIN_NODE_INFO as Windows lays it out: a name that fills the whole array has no NUL, and is read
// to its end and no further; a thread node's IDs come from ThreadObject.
TEST(WindowsWaitChainDecodeTest, NodesAreReadFromTheUnionMemberTheirTypeUses)
{
    const std::wstring longName(WCT_OBJNAME_LENGTH, L'x');
    std::array<WAITCHAIN_NODE_INFO, 3> nodes{
        threadInfo(5, 6, WctStatusBlocked), objectInfo(WctAlpcType, WctStatusOwned, longName), threadInfo(8, 0, WctStatusPidOnly)};
    const ThreadWaitChain chain = decodeWaitChainNodes(6, nodes, true, false);
    EXPECT_EQ(chain.tid, 6);
    EXPECT_TRUE(chain.isCycle);
    ASSERT_EQ(chain.nodes.size(), 3U);
    EXPECT_EQ(chain.nodes[0].pid, 5);
    EXPECT_EQ(chain.nodes[0].tid, 6);
    EXPECT_EQ(chain.nodes[1].type, WaitObjectType::Alpc);
    EXPECT_EQ(chain.nodes[1].objectName, std::string(WCT_OBJNAME_LENGTH, 'x'));
    EXPECT_EQ(chain.nodes[2].status, WaitNodeStatus::PidOnly);
    EXPECT_EQ(chain.nodes[2].pid, 8);

    // A non-ASCII name arrives as UTF-8.
    const std::array<WAITCHAIN_NODE_INFO, 1> named{objectInfo(WctMutexType, WctStatusNotOwned, L"Sperre-Ä")};
    EXPECT_EQ(decodeWaitChainNodes(1, named, false, false).nodes.front().objectName, "Sperre-\xC3\x84");
}

// The real listing, of this process: its own threads, and its own name among every process's.
TEST(WindowsWaitChainRealTest, ListsThisProcesssThreadsAndNames)
{
    const std::optional<ThreadListing> listing = listProcessThreads(GetCurrentProcessId());
    ASSERT_TRUE(listing.has_value());
    EXPECT_TRUE(std::ranges::find(listing->threadIds, GetCurrentThreadId()) != listing->threadIds.end());
    const auto self = std::ranges::find(listing->processNames, GetCurrentProcessId(), &std::pair<DWORD, std::string>::first);
    ASSERT_NE(self, listing->processNames.end());
    EXPECT_TRUE(self->second.ends_with(".exe")) << self->second;
    EXPECT_TRUE(registerWaitChainComCallbacks() == registerWaitChainComCallbacks()) << "registered once, and the answer kept";
}

// =============================================================================
// One real read, of a child process this test starts: this test binary again, running only the
// WaitChainChild test below, which blocks on a mutex this test holds. Its chain must lead from its
// thread through the mutex to this process's thread. Nothing else is read, and the child is ended
// by the test either way.
// =============================================================================

constexpr const wchar_t* CHILD_MUTEX_VARIABLE = L"TASKSMACK_WAIT_CHAIN_TEST_MUTEX";

/// Run by the child only: waits on the mutex its parent named, which the parent holds. Skipped in any
/// ordinary run, where no mutex is named.
TEST(WaitChainChild, BlocksOnTheMutexItsParentHolds)
{
    std::array<wchar_t, 128> name{};
    if (GetEnvironmentVariableW(CHILD_MUTEX_VARIABLE, name.data(), static_cast<DWORD>(name.size())) == 0)
    {
        GTEST_SKIP() << "Only the wait chain test's child runs this";
    }
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.data());
    ASSERT_NE(mutex, nullptr);
    if (WaitForSingleObject(mutex, 60000) == WAIT_OBJECT_0)
    {
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
}

/// The child: started with the mutex's name in its environment, ended on scope exit if it hasn't exited.
class BlockedChild
{
  public:
    explicit BlockedChild(const std::wstring& mutexName)
    {
        std::array<wchar_t, MAX_PATH> self{};
        if (GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())) == 0)
        {
            return;
        }
        std::wstring commandLine = L"\"" + std::wstring(self.data()) + L"\" --gtest_filter=WaitChainChild.BlocksOnTheMutexItsParentHolds";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        SetEnvironmentVariableW(CHILD_MUTEX_VARIABLE, mutexName.c_str());
        if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &m_Info) ==
            0)
        {
            m_Info = {};
        }
        SetEnvironmentVariableW(CHILD_MUTEX_VARIABLE, nullptr);
    }
    BlockedChild(const BlockedChild&) = delete;
    BlockedChild& operator=(const BlockedChild&) = delete;
    BlockedChild(BlockedChild&&) = delete;
    BlockedChild& operator=(BlockedChild&&) = delete;
    ~BlockedChild()
    {
        if (m_Info.hProcess != nullptr)
        {
            if (WaitForSingleObject(m_Info.hProcess, 10000) != WAIT_OBJECT_0)
            {
                TerminateProcess(m_Info.hProcess, 1); // only ever this test's own child
                WaitForSingleObject(m_Info.hProcess, 5000);
            }
            CloseHandle(m_Info.hThread);
            CloseHandle(m_Info.hProcess);
        }
    }

    [[nodiscard]] bool started() const
    {
        return m_Info.hProcess != nullptr;
    }
    [[nodiscard]] ProcessTarget target() const
    {
        FILETIME creation{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        GetProcessTimes(m_Info.hProcess, &creation, &exitTime, &kernelTime, &userTime);
        return {.pid = static_cast<std::int32_t>(m_Info.dwProcessId),
                .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime};
    }

  private:
    PROCESS_INFORMATION m_Info{};
};

/// The chain of @p result that waits on a mutex, if any thread's does yet.
[[nodiscard]] const ThreadWaitChain* mutexChain(const WaitChainReadResult& result)
{
    for (const ThreadWaitChain& chain : result.threads)
    {
        if (chain.nodes.size() >= 2 && chain.nodes[1].type == WaitObjectType::Mutex)
        {
            return &chain;
        }
    }
    return nullptr;
}

TEST(WindowsWaitChainRealTest, AChildBlockedOnOurMutexLeadsBackToThisThread)
{
    const std::wstring mutexName =
        L"Local\\TaskSmackWaitChainTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str()); // held by this thread
    ASSERT_NE(mutex, nullptr);
    std::optional<WaitChainReadResult> found;
    {
        const BlockedChild child(mutexName);
        if (!child.started())
        {
            CloseHandle(mutex);
            GTEST_SKIP() << "Couldn't start the child: " << GetLastError();
        }
        WindowsProcessActions actions;
        ASSERT_TRUE(actions.actionCapabilities().canAnalyzeWaitChain);
        const ProcessTarget target = child.target();
        // The child's start-up takes a moment; read until its thread waits on the mutex (15 s at most).
        for (int attempt = 0; attempt < 150 && !found.has_value(); ++attempt)
        {
            WaitChainReadResult result = actions.readWaitChains(target, std::stop_token{});
            ASSERT_EQ(result.status, WaitChainReadStatus::Ok) << result.detail;
            if (mutexChain(result) != nullptr)
            {
                found = std::move(result);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ReleaseMutex(mutex); // lets the child finish; ~BlockedChild waits for it
    }
    CloseHandle(mutex);

    ASSERT_TRUE(found.has_value()) << "the child never waited on the mutex";
    const ThreadWaitChain* chain = mutexChain(*found);
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(chain->nodes[0].type, WaitObjectType::Thread);
    EXPECT_EQ(chain->nodes[0].status, WaitNodeStatus::Blocked);
    EXPECT_NE(chain->nodes[1].objectName.find("TaskSmackWaitChainTest"), std::string::npos) << chain->nodes[1].objectName;
    EXPECT_EQ(chain->nodes[1].status, WaitNodeStatus::Owned);
    ASSERT_GE(chain->nodes.size(), 3U);
    EXPECT_EQ(chain->nodes[2].type, WaitObjectType::Thread);
    EXPECT_EQ(chain->nodes[2].pid, static_cast<std::int32_t>(GetCurrentProcessId())) << "the owner is this process";
    EXPECT_EQ(chain->nodes[2].tid, static_cast<std::int32_t>(GetCurrentThreadId())) << "and this thread, which made the mutex";
    EXPECT_FALSE(chain->nodes[2].processName.empty());
    EXPECT_NE(chain->nodes[2].processStartTimeTicks, 0U);
    EXPECT_FALSE(chain->isCycle);
}

} // namespace
} // namespace Platform::Windows
