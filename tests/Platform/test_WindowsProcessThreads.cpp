/// @file test_WindowsProcessThreads.cpp
/// @brief Process Details' Threads section on Windows (#3069): the SystemProcessInformation snapshot's parse,
/// the state and wait-reason tables, the reader through fake calls (snapshots built here, fake thread handles),
/// and one real read of this test process's own threads, one of them suspended by the test itself.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "Platform/Windows/WindowsProcessThreads.h"
#include "Platform/Windows/WindowsProcessThreadsMath.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// One thread of a process in a snapshot built here.
struct FakeThread
{
    std::int32_t tid = 0;
    std::uint32_t state = 5;
    std::uint32_t waitReason = 6;
    std::int32_t priority = 8;
    std::uint64_t kernelTime = 0;
    std::uint64_t userTime = 0;
    std::uint64_t createTime = 0;
};

struct FakeProcess
{
    std::uint32_t pid = 0;
    std::uint64_t createTime = 0;
    std::vector<FakeThread> threads;
    /// The NumberOfThreads the entry claims, when not threads.size() (a malformed entry).
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::optional<ULONG> claimedThreads{};
};

/// A SystemProcessInformation snapshot as the kernel writes one: each entry, then its thread records.
[[nodiscard]] std::vector<std::byte> buildSnapshot(const std::vector<FakeProcess>& processes)
{
    std::vector<std::byte> bytes;
    for (std::size_t p = 0; p < processes.size(); ++p)
    {
        const FakeProcess& process = processes[p];
        const std::size_t entrySize = sizeof(SYSTEM_PROCESS_INFORMATION) + (process.threads.size() * sizeof(SYSTEM_THREAD_INFORMATION));
        const std::size_t offset = bytes.size();
        bytes.resize(offset + entrySize);
        SYSTEM_PROCESS_INFORMATION entry{};
        entry.NextEntryOffset = (p + 1 < processes.size()) ? static_cast<ULONG>(entrySize) : 0;
        entry.NumberOfThreads = process.claimedThreads.value_or(static_cast<ULONG>(process.threads.size()));
        entry.UniqueProcessId = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(process.pid));
        std::memcpy(&bytes[offset], &entry, sizeof(entry));
        const auto createTime = static_cast<LONGLONG>(process.createTime);
        std::memcpy(&bytes[offset + PROCESS_CREATE_TIME_OFFSET], &createTime, sizeof(createTime));
        for (std::size_t t = 0; t < process.threads.size(); ++t)
        {
            const FakeThread& thread = process.threads[t];
            SYSTEM_THREAD_INFORMATION record{};
            record.Reserved1[0].QuadPart = static_cast<LONGLONG>(thread.kernelTime);
            record.Reserved1[1].QuadPart = static_cast<LONGLONG>(thread.userTime);
            record.Reserved1[2].QuadPart = static_cast<LONGLONG>(thread.createTime);
            record.ClientId.UniqueProcess = entry.UniqueProcessId;
            record.ClientId.UniqueThread = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(thread.tid));
            record.Priority = thread.priority;
            record.ThreadState = thread.state;
            record.WaitReason = thread.waitReason;
            std::memcpy(&bytes[offset + sizeof(SYSTEM_PROCESS_INFORMATION) + (t * sizeof(record))], &record, sizeof(record));
        }
    }
    return bytes;
}

// 2026-01-01 00:00:00 UTC as a FILETIME, plus half a second: the boot the fakes report.
constexpr std::uint64_t BOOT_FILETIME = 134116992000000000ULL + 5'000'000ULL;
constexpr std::uint64_t BOOT_EPOCH = 1'767'225'600ULL;
constexpr std::uint64_t PROCESS_CREATED = BOOT_FILETIME + 600'000'000ULL; // a minute after boot
constexpr std::uint32_t PID = 4321;

TEST(WindowsProcessThreadsMathTest, EachSchedulerStateHasItsLetter)
{
    struct Case
    {
        std::uint32_t state;
        std::uint32_t waitReason;
        char letter;
    };
    for (const Case& c : {Case{.state = 0, .waitReason = 0, .letter = 'R'},  // Initialized
                          Case{.state = 1, .waitReason = 0, .letter = 'R'},  // Ready
                          Case{.state = 2, .waitReason = 0, .letter = 'R'},  // Running
                          Case{.state = 3, .waitReason = 0, .letter = 'R'},  // Standby
                          Case{.state = 4, .waitReason = 0, .letter = 'X'},  // Terminated
                          Case{.state = 5, .waitReason = 6, .letter = 'S'},  // Waiting: UserRequest
                          Case{.state = 5, .waitReason = 5, .letter = 'T'},  // Waiting: Suspended
                          Case{.state = 5, .waitReason = 12, .letter = 'T'}, // Waiting: WrSuspended
                          Case{.state = 6, .waitReason = 0, .letter = 'R'},  // Transition
                          Case{.state = 7, .waitReason = 0, .letter = 'R'},  // DeferredReady
                          Case{.state = 8, .waitReason = 0, .letter = 'S'},  // GateWaitObsolete
                          Case{.state = 9, .waitReason = 0, .letter = 'S'},  // WaitingForProcessInSwap
                          Case{.state = 42, .waitReason = 0, .letter = '?'}})
    {
        EXPECT_EQ(threadStateLetter(c.state, c.waitReason), c.letter) << c.state << "/" << c.waitReason;
    }
}

TEST(WindowsProcessThreadsMathTest, WaitReasonsAreNamedAsTheKernelNamesThem)
{
    EXPECT_EQ(waitReasonName(0), "Executive");
    EXPECT_EQ(waitReasonName(5), "Suspended");
    EXPECT_EQ(waitReasonName(6), "UserRequest");
    EXPECT_EQ(waitReasonName(13), "WrUserRequest");
    EXPECT_EQ(waitReasonName(15), "WrQueue");
    EXPECT_EQ(waitReasonName(37), "WrAlertByThreadId");
    EXPECT_EQ(waitReasonName(42), "WrRcu");
    EXPECT_EQ(waitReasonName(43), "Reason 43") << "past the table: its number";
    EXPECT_TRUE(isSuspendedWaitReason(5));
    EXPECT_TRUE(isSuspendedWaitReason(12));
    EXPECT_FALSE(isSuspendedWaitReason(6));
}

TEST(WindowsProcessThreadsMathTest, TheBootTimeCountsFromItsWholeSecond)
{
    const BootTimeBase boot = bootTimeBase(BOOT_FILETIME);
    EXPECT_EQ(boot.epochSeconds, BOOT_EPOCH);
    EXPECT_EQ(boot.fileTime, BOOT_FILETIME - 5'000'000ULL);
    EXPECT_EQ(bootTimeBase(0).epochSeconds, 0U) << "unknown";
    EXPECT_EQ(bootTimeBase(116444736000000000ULL).fileTime, 0U) << "1970 itself: an unset clock";
}

TEST(WindowsProcessThreadsMathTest, ARecordBecomesARowWithItsStartCountedFromBoot)
{
    const BootTimeBase boot = bootTimeBase(BOOT_FILETIME);
    const ProcessThread waiting = toProcessThread(
        {.tid = 77, .state = 5, .waitReason = 6, .priority = 10, .kernelTime = 30, .userTime = 40, .createTime = boot.fileTime + 123},
        boot);
    EXPECT_EQ(waiting.tid, 77);
    EXPECT_EQ(waiting.state, 'S');
    EXPECT_EQ(waiting.waitReason, "UserRequest");
    EXPECT_EQ(waiting.nice, 10) << "Windows' priority";
    EXPECT_EQ(waiting.systemTicks, 30U);
    EXPECT_EQ(waiting.userTicks, 40U);
    EXPECT_EQ(waiting.startTicks, 123U);
    EXPECT_EQ(waiting.createTime, boot.fileTime + 123) << "its FILETIME, for a thread action's identity check";
    EXPECT_TRUE(waiting.name.empty()) << "the reader's, from a handle";

    const ProcessThread running = toProcessThread({.tid = 78, .state = 2, .waitReason = 6, .createTime = 5}, boot);
    EXPECT_TRUE(running.waitReason.empty()) << "only a waiting thread has a wait reason";
    EXPECT_EQ(running.startTicks, 0U) << "before the boot: unknown";
    EXPECT_EQ(toProcessThread({.tid = 79, .createTime = 99}, BootTimeBase{}).startTicks, 0U) << "no boot time";
}

TEST(WindowsProcessThreadsMathTest, FindsTheProcessAmongOthersAndReadsItsThreads)
{
    const std::vector<std::byte> snapshot = buildSnapshot({
        {.pid = 0, .createTime = 0, .threads = {{.tid = 0}}},
        {.pid = PID,
         .createTime = PROCESS_CREATED,
         .threads = {{.tid = 100, .state = 2, .priority = 9, .kernelTime = 1, .userTime = 2, .createTime = 3},
                     {.tid = 104, .state = 5, .waitReason = 5}}},
        {.pid = 9999, .createTime = 1, .threads = {{.tid = 500}}},
    });
    const SnapshotProcess found = findProcessThreads(snapshot, PID, 100);
    ASSERT_EQ(found.status, SnapshotProcess::Status::Found);
    EXPECT_EQ(found.createTime, PROCESS_CREATED);
    EXPECT_FALSE(found.truncated);
    ASSERT_EQ(found.threads.size(), 2U);
    EXPECT_EQ(found.threads[0].tid, 100);
    EXPECT_EQ(found.threads[0].state, 2U);
    EXPECT_EQ(found.threads[0].priority, 9);
    EXPECT_EQ(found.threads[0].kernelTime, 1U);
    EXPECT_EQ(found.threads[0].userTime, 2U);
    EXPECT_EQ(found.threads[0].createTime, 3U);
    EXPECT_EQ(found.threads[1].waitReason, 5U);

    EXPECT_EQ(findProcessThreads(snapshot, 9999, 100).threads.at(0).tid, 500) << "the last entry";
    EXPECT_EQ(findProcessThreads(snapshot, 1234, 100).status, SnapshotProcess::Status::NotFound);
    EXPECT_EQ(findProcessThreads({}, PID, 100).status, SnapshotProcess::Status::NotFound) << "nothing written";
}

TEST(WindowsProcessThreadsMathTest, ATooLongThreadArrayIsMalformedAndAShortBufferEndsTheWalk)
{
    std::vector<std::byte> snapshot =
        buildSnapshot({{.pid = PID, .createTime = PROCESS_CREATED, .threads = {{.tid = 1}}, .claimedThreads = 50}});
    EXPECT_EQ(findProcessThreads(snapshot, PID, 100).status, SnapshotProcess::Status::Malformed);

    // The second entry is cut short: the walk stops at the bytes written, never past them.
    snapshot =
        buildSnapshot({{.pid = 1, .createTime = 1, .threads = {{.tid = 1}}}, {.pid = PID, .createTime = 2, .threads = {{.tid = 2}}}});
    snapshot.resize(snapshot.size() - 8);
    const SnapshotProcess cut = findProcessThreads(snapshot, PID, 100);
    EXPECT_NE(cut.status, SnapshotProcess::Status::Found);
}

TEST(WindowsProcessThreadsMathTest, ThreadsPastTheCapAreLeftOut)
{
    const std::vector<std::byte> snapshot =
        buildSnapshot({{.pid = PID, .createTime = PROCESS_CREATED, .threads = {{.tid = 1}, {.tid = 2}, {.tid = 3}}}});
    const SnapshotProcess found = findProcessThreads(snapshot, PID, 2);
    ASSERT_EQ(found.status, SnapshotProcess::Status::Found);
    EXPECT_TRUE(found.truncated);
    ASSERT_EQ(found.threads.size(), 2U);
    EXPECT_EQ(found.threads[1].tid, 2);
}

/// The fakes' shared state: function pointers can't capture. Each test starts from reset().
struct FakeWin
{
    static inline std::vector<std::byte> snapshot;
    static inline LONG queryStatus = 0; // NtQuerySystemInformation(SystemProcessInformation)'s answer, when the buffer is big enough
    static inline LONG timeStatus = 0;  // ...and SystemTimeOfDayInformation's
    static inline int mismatches = 0;   // answer STATUS_INFO_LENGTH_MISMATCH this many times first
    static inline int processQueries = 0;
    static inline std::int32_t deniedTid = 0;  // OpenThread refuses this one
    static inline std::int32_t unnamedTid = 0; // its description is empty
    static inline std::int32_t suspendedTid = 0;
    static inline std::int32_t reusedTid = 0;                        // its TID now names a newer thread than the snapshot's
    static inline std::int32_t otherProcessTid = 0;                  // its TID now names a thread of another process
    static inline std::map<std::int32_t, std::uint64_t> createTimes; // each thread's, as GetThreadTimes answers
    static inline int opens = 0;
    static inline int closes = 0;
    static inline double snapshotAt = 0.0;           // when the last full snapshot was handed back (#3337)
    static inline std::optional<double> firstOpenAt; // when the first thread was opened after it

    static void reset()
    {
        snapshot.clear();
        queryStatus = 0;
        timeStatus = 0;
        mismatches = 0;
        processQueries = 0;
        deniedTid = 0;
        unnamedTid = 0;
        suspendedTid = 0;
        reusedTid = 0;
        otherProcessTid = 0;
        createTimes.clear();
        opens = 0;
        closes = 0;
        snapshotAt = 0.0;
        firstOpenAt.reset();
    }

    static LONG NTAPI query(ULONG infoClass, PVOID buffer, ULONG length, PULONG returned)
    {
        if (infoClass == SYSTEM_TIME_OF_DAY_INFORMATION_CLASS)
        {
            if (timeStatus < 0)
            {
                return timeStatus;
            }
            const auto boot = static_cast<LONGLONG>(BOOT_FILETIME);
            std::memcpy(buffer, &boot, sizeof(boot));
            *returned = length;
            return 0;
        }
        ++processQueries;
        if (mismatches > 0 || length < snapshot.size())
        {
            --mismatches;
            *returned = static_cast<ULONG>(snapshot.size());
            return static_cast<LONG>(0xC0000004L);
        }
        if (queryStatus < 0)
        {
            return queryStatus;
        }
        std::memcpy(buffer, snapshot.data(), snapshot.size());
        *returned = static_cast<ULONG>(snapshot.size());
        snapshotAt = threadsReadClockSeconds();
        firstOpenAt.reset();
        return 0;
    }

    /// A handle that is only a number: the TID, offset so it is never null. Never a real handle.
    static HANDLE WINAPI openThread(DWORD /*access*/, BOOL /*inherit*/, DWORD tid)
    {
        ++opens;
        if (!firstOpenAt.has_value())
        {
            firstOpenAt = threadsReadClockSeconds();
        }
        if (static_cast<std::int32_t>(tid) == deniedTid)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x10000U + tid));
    }
    static BOOL WINAPI closeHandle(HANDLE /*handle*/)
    {
        ++closes;
        return TRUE;
    }
    static std::int32_t tidOf(HANDLE handle)
    {
        return static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(handle) - 0x10000U);
    }
    static DWORD WINAPI getProcessIdOfThread(HANDLE thread)
    {
        return tidOf(thread) == otherProcessTid ? PID + 1 : PID;
    }
    static BOOL WINAPI getThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        const std::int32_t tid = tidOf(thread);
        const std::uint64_t time = createTimes[tid] + (tid == reusedTid ? 1U : 0U);
        creation->dwLowDateTime = static_cast<DWORD>(time & 0xFFFF'FFFFU);
        creation->dwHighDateTime = static_cast<DWORD>(time >> 32U);
        return TRUE;
    }
    static HRESULT WINAPI getThreadDescription(HANDLE thread, PWSTR* description)
    {
        const std::wstring text = tidOf(thread) == unnamedTid ? std::wstring() : L"worker-" + std::to_wstring(tidOf(thread));
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        auto* copy = static_cast<PWSTR>(LocalAlloc(LMEM_FIXED, bytes));
        std::memcpy(copy, text.c_str(), bytes);
        *description = copy;
        return S_OK;
    }
    static LONG NTAPI queryThread(HANDLE thread, ULONG infoClass, PVOID info, ULONG length, PULONG /*returned*/)
    {
        if (infoClass != THREAD_SUSPEND_COUNT_CLASS || length != sizeof(ULONG))
        {
            return static_cast<LONG>(0xC0000003L); // STATUS_INVALID_INFO_CLASS
        }
        *static_cast<ULONG*>(info) = tidOf(thread) == suspendedTid ? 2U : 0U;
        return 0;
    }

    [[nodiscard]] static ProcessThreadFunctions functions()
    {
        return {.ntQuerySystemInformation = &query,
                .openThread = &openThread,
                .closeHandle = &closeHandle,
                .getThreadDescription = &getThreadDescription,
                .localFree = &LocalFree,
                .ntQueryInformationThread = &queryThread,
                .getProcessIdOfThread = &getProcessIdOfThread,
                .getThreadTimes = &getThreadTimes};
    }
};

class WindowsProcessThreadsReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeWin::reset();
        FakeWin::snapshot = buildSnapshot({
            {.pid = 4, .createTime = 1, .threads = {{.tid = 8}}},
            {.pid = PID,
             .createTime = PROCESS_CREATED,
             .threads = {{.tid = 100, .state = 2, .priority = 10, .kernelTime = 20, .userTime = 30, .createTime = PROCESS_CREATED},
                         {.tid = 104, .state = 5, .waitReason = 5, .createTime = PROCESS_CREATED + 10'000'000ULL},
                         {.tid = 108, .state = 5, .waitReason = 15},
                         {.tid = 112, .state = 5, .waitReason = 6}}},
        });
        FakeWin::createTimes = {{100, PROCESS_CREATED}, {104, PROCESS_CREATED + 10'000'000ULL}, {108, 0}, {112, 0}};
    }
};

constexpr ProcessTarget TARGET{.pid = static_cast<std::int32_t>(PID), .startTimeTicks = PROCESS_CREATED};

TEST_F(WindowsProcessThreadsReaderTest, ListsEachThreadWithItsNameStateAndSuspendCount)
{
    FakeWin::unnamedTid = 108;
    FakeWin::deniedTid = 112;
    FakeWin::suspendedTid = 104;
    WindowsProcessThreadsReader reader(FakeWin::functions());
    ASSERT_TRUE(reader.hasThreads());

    const ThreadsReadResult result = reader.readThreads(TARGET);
    ASSERT_EQ(result.status, ThreadsReadStatus::Ok) << result.detail;
    EXPECT_TRUE(result.windowsPriorities);
    EXPECT_EQ(result.ticksPerSecond, 10'000'000);
    EXPECT_EQ(result.bootTimeEpoch, BOOT_EPOCH);
    EXPECT_FALSE(result.truncated);
    ASSERT_EQ(result.threads.size(), 4U);

    const ProcessThread& main = result.threads[0];
    EXPECT_EQ(main.tid, 100);
    EXPECT_EQ(main.name, "worker-100");
    EXPECT_EQ(main.state, 'R');
    EXPECT_EQ(main.nice, 10);
    EXPECT_EQ(main.systemTicks, 20U);
    EXPECT_EQ(main.userTicks, 30U);
    EXPECT_EQ(main.startTicks, PROCESS_CREATED - bootTimeBase(BOOT_FILETIME).fileTime);
    EXPECT_EQ(main.suspendCount, 0U);
    EXPECT_EQ(main.createTime, PROCESS_CREATED) << "the identity a thread action checks";

    EXPECT_EQ(result.threads[1].state, 'T');
    EXPECT_EQ(result.threads[1].waitReason, "Suspended");
    EXPECT_EQ(result.threads[1].suspendCount, 2U);
    EXPECT_TRUE(result.threads[2].name.empty()) << "an empty description is no name";
    EXPECT_EQ(result.threads[2].waitReason, "WrQueue");
    EXPECT_TRUE(result.threads[3].name.empty()) << "a thread that refuses the open is listed without one";
    EXPECT_EQ(result.threads[3].state, 'S');

    EXPECT_EQ(FakeWin::opens, 4);
    EXPECT_EQ(FakeWin::closes, 3) << "every handle opened is closed";
}

// #3069: a thread that exited between the snapshot and its open may have left its TID to a newer thread, of this
// process or another: that one's name and suspend count aren't shown on the old row.
TEST_F(WindowsProcessThreadsReaderTest, ATidReusedSinceTheSnapshotGetsNoNameOrSuspendCount)
{
    FakeWin::suspendedTid = 104;
    FakeWin::reusedTid = 104;
    FakeWin::otherProcessTid = 100;
    WindowsProcessThreadsReader reader(FakeWin::functions());
    const ThreadsReadResult result = reader.readThreads(TARGET);
    ASSERT_EQ(result.status, ThreadsReadStatus::Ok);
    ASSERT_EQ(result.threads.size(), 4U);
    EXPECT_TRUE(result.threads[0].name.empty()) << "TID 100 is another process's thread now";
    EXPECT_TRUE(result.threads[1].name.empty()) << "TID 104 names a newer thread";
    EXPECT_EQ(result.threads[1].suspendCount, 0U) << "nor its suspend count, which would offer Resume";
    EXPECT_EQ(result.threads[1].state, 'T') << "the snapshot's own figures stay";
    EXPECT_EQ(result.threads[2].name, "worker-108") << "a thread still the snapshot's keeps its name";
    EXPECT_EQ(FakeWin::closes, FakeWin::opens) << "every handle opened is closed, the mismatched ones too";
}

TEST_F(WindowsProcessThreadsReaderTest, AReusedPidOrAGoneProcessReadsAsExited)
{
    WindowsProcessThreadsReader reader(FakeWin::functions());
    EXPECT_EQ(reader.readThreads({.pid = TARGET.pid, .startTimeTicks = PROCESS_CREATED + 1}).status, ThreadsReadStatus::ProcessExited);
    EXPECT_EQ(reader.readThreads({.pid = 5555, .startTimeTicks = 7}).status, ThreadsReadStatus::ProcessExited);
    EXPECT_EQ(reader.readThreads({.pid = -1, .startTimeTicks = 7}).status, ThreadsReadStatus::ProcessExited);
    EXPECT_EQ(reader.readThreads({.pid = TARGET.pid, .startTimeTicks = 0}).status, ThreadsReadStatus::IdentityUnknown);
    EXPECT_EQ(FakeWin::opens, 0) << "no thread of the wrong process is opened";
}

TEST_F(WindowsProcessThreadsReaderTest, TheSnapshotGrowsUntilItFits)
{
    FakeWin::mismatches = 2;
    WindowsProcessThreadsReader reader(FakeWin::functions());
    EXPECT_EQ(reader.readThreads(TARGET).status, ThreadsReadStatus::Ok);
    EXPECT_EQ(FakeWin::processQueries, 3);

    FakeWin::processQueries = 0;
    FakeWin::mismatches = 10; // the list keeps outgrowing the buffer: given up on, not looped on
    const ThreadsReadResult result = reader.readThreads(TARGET);
    EXPECT_EQ(result.status, ThreadsReadStatus::Failed);
    EXPECT_EQ(FakeWin::processQueries, 4);
}

TEST_F(WindowsProcessThreadsReaderTest, AFailedQueryIsFailedAndAnUnreadableBootTimeIsUnknown)
{
    FakeWin::queryStatus = static_cast<LONG>(0xC0000022L);
    WindowsProcessThreadsReader reader(FakeWin::functions());
    const ThreadsReadResult failed = reader.readThreads(TARGET);
    EXPECT_EQ(failed.status, ThreadsReadStatus::Failed);
    EXPECT_EQ(failed.detail, "NtQuerySystemInformation failed: NTSTATUS 0xC0000022");

    FakeWin::queryStatus = 0;
    FakeWin::timeStatus = static_cast<LONG>(0xC0000001L);
    const ThreadsReadResult noBoot = reader.readThreads(TARGET);
    ASSERT_EQ(noBoot.status, ThreadsReadStatus::Ok);
    EXPECT_EQ(noBoot.bootTimeEpoch, 0U) << "STARTED shows the dash";
    EXPECT_EQ(noBoot.threads[0].startTicks, 0U);
}

// #3337: the read is stamped when its CPU times were read, the snapshot, before the per-thread opens whose time
// varies: not when the read returns.
TEST_F(WindowsProcessThreadsReaderTest, TheReadIsStampedAtTheSnapshotBeforeTheThreadDetails)
{
    WindowsProcessThreadsReader reader(FakeWin::functions());
    const double before = threadsReadClockSeconds();
    const ThreadsReadResult result = reader.readThreads(TARGET);
    ASSERT_EQ(result.status, ThreadsReadStatus::Ok) << result.detail;
    ASSERT_TRUE(result.acquiredAtSeconds.has_value());
    ASSERT_TRUE(FakeWin::firstOpenAt.has_value());
    const double stamp = result.acquiredAtSeconds.value_or(-1.0);
    EXPECT_GE(stamp, before);
    EXPECT_GE(stamp, FakeWin::snapshotAt) << "after the snapshot";
    EXPECT_LE(stamp, FakeWin::firstOpenAt.value_or(-1.0)) << "before the first thread's details";

    FakeWin::queryStatus = static_cast<LONG>(0xC0000022L);
    EXPECT_FALSE(reader.readThreads(TARGET).acquiredAtSeconds.has_value()) << "a failed read has no CPU times to stamp";
}

TEST_F(WindowsProcessThreadsReaderTest, AMalformedSnapshotFails)
{
    FakeWin::snapshot = buildSnapshot({{.pid = PID, .createTime = PROCESS_CREATED, .threads = {{.tid = 1}}, .claimedThreads = 9}});
    WindowsProcessThreadsReader reader(FakeWin::functions());
    const ThreadsReadResult result = reader.readThreads(TARGET);
    EXPECT_EQ(result.status, ThreadsReadStatus::Failed);
    EXPECT_EQ(result.detail, "The system's process list was malformed");
}

TEST_F(WindowsProcessThreadsReaderTest, WithoutTheOptionalCallsItListsThreadsWithoutNamesOrCounts)
{
    ProcessThreadFunctions api = FakeWin::functions();
    api.getThreadDescription = nullptr;
    api.ntQueryInformationThread = nullptr;
    FakeWin::suspendedTid = 104;
    WindowsProcessThreadsReader reader(api);
    const ThreadsReadResult result = reader.readThreads(TARGET);
    ASSERT_EQ(result.status, ThreadsReadStatus::Ok);
    EXPECT_TRUE(std::ranges::all_of(result.threads, [](const ProcessThread& t) { return t.name.empty() && t.suspendCount == 0; }));

    api.ntQuerySystemInformation = nullptr;
    WindowsProcessThreadsReader none(api);
    EXPECT_FALSE(none.hasThreads());
    EXPECT_EQ(none.readThreads(TARGET).status, ThreadsReadStatus::Unsupported);
}

/// A thread of this test process that waits until told to stop: what the real read finds, named, and
/// suspended by the test itself (never another process's thread).
class OwnWaitingThread
{
  public:
    OwnWaitingThread()
        : m_Stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_Started(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_Thread(CreateThread(nullptr, 0, &OwnWaitingThread::run, this, 0, &m_Tid))
    {
        SetThreadDescription(m_Thread, L"ts-test-waiter");
    }
    ~OwnWaitingThread()
    {
        // A failed assertion may have left it suspended, which the stop event can't wake: resume it fully first,
        // so a regression reports a failure instead of hanging here (ResumeThread answers the count it had).
        DWORD previous = 0;
        do
        {
            previous = ResumeThread(m_Thread);
        } while (previous != static_cast<DWORD>(-1) && previous > 1);
        SetEvent(m_Stop);
        WaitForSingleObject(m_Thread, INFINITE);
        CloseHandle(m_Thread);
        CloseHandle(m_Started);
        CloseHandle(m_Stop);
    }
    OwnWaitingThread(const OwnWaitingThread&) = delete;
    OwnWaitingThread& operator=(const OwnWaitingThread&) = delete;
    OwnWaitingThread(OwnWaitingThread&&) = delete;
    OwnWaitingThread& operator=(OwnWaitingThread&&) = delete;

    [[nodiscard]] HANDLE handle() const noexcept
    {
        return m_Thread;
    }
    /// Waits until the thread runs its own code, so suspending it can't leave the loader lock held.
    [[nodiscard]] bool waitStarted() const noexcept
    {
        return WaitForSingleObject(m_Started, 5000) == WAIT_OBJECT_0;
    }
    [[nodiscard]] std::int32_t tid() const noexcept
    {
        return static_cast<std::int32_t>(m_Tid);
    }

  private:
    static DWORD WINAPI run(LPVOID self)
    {
        const auto* thread = static_cast<const OwnWaitingThread*>(self);
        SetEvent(thread->m_Started);
        WaitForSingleObject(thread->m_Stop, INFINITE);
        return 0;
    }

    HANDLE m_Stop = nullptr;
    HANDLE m_Started = nullptr; // set once the thread runs: past its start-up, which holds the loader lock
    DWORD m_Tid = 0;
    HANDLE m_Thread = nullptr;
};

[[nodiscard]] ProcessTarget ownTarget()
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return {.pid = static_cast<std::int32_t>(GetCurrentProcessId()),
            .startTimeTicks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime};
}

TEST(WindowsProcessThreadsRealTest, ReadsThisProcesssOwnThreads)
{
    const OwnWaitingThread waiter;
    ASSERT_NE(waiter.handle(), nullptr);
    ASSERT_TRUE(waiter.waitStarted());
    ASSERT_NE(SuspendThread(waiter.handle()), static_cast<DWORD>(-1)); // this test's own thread, resumed below

    WindowsProcessThreadsReader reader;
    ASSERT_TRUE(reader.hasThreads());
    const ThreadsReadResult result = reader.readThreads(ownTarget());
    ResumeThread(waiter.handle());

    ASSERT_EQ(result.status, ThreadsReadStatus::Ok) << result.detail;
    EXPECT_TRUE(result.windowsPriorities);
    EXPECT_NE(result.bootTimeEpoch, 0U);
    const auto self = std::ranges::find(result.threads, static_cast<std::int32_t>(GetCurrentThreadId()), &ProcessThread::tid);
    ASSERT_NE(self, result.threads.end()) << "the thread reading is listed";
    EXPECT_EQ(self->state, 'R') << "it was running the read";
    const auto found = std::ranges::find(result.threads, waiter.tid(), &ProcessThread::tid);
    ASSERT_NE(found, result.threads.end());
    EXPECT_EQ(found->name, "ts-test-waiter");
    EXPECT_EQ(found->suspendCount, 1U);
    EXPECT_GE(found->nice, 1);
    EXPECT_LE(found->nice, 31);
}

} // namespace
} // namespace Platform::Windows
