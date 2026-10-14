/// @file test_WindowsProcessOpenFiles.cpp
/// @brief Platform::Windows::WindowsProcessOpenFilesReader (#183) against a fake handle table and fake
/// kernel32 calls: only the target's File handles listed, typed and named (disks only), the File type
/// learned once, hang-prone access masks never touched, a handle that never answers abandoned under the
/// timeout and skipped after, the count cap, a denied open, a reused PID, every handle duplicated with
/// DUPLICATE_SAME_ACCESS and closed; long, NT-only and unnamable paths, failed calls and too many stuck namers.
/// Plus one read of this test process through the real calls.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/Windows/WindowsProcessOpenFiles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = START_TICKS};
constexpr DWORD SELF_PID = 77;
constexpr USHORT FILE_TYPE_INDEX = 37;
constexpr ULONG_PTR NUL_HANDLE = 0x9990;
constexpr ULONG READ_WRITE_ACCESS = 0x0012019F;

/// One handle in the fake table, and what the fakes answer for it.
struct FakeHandle
{
    ULONG_PTR pid = TARGET.pid;
    ULONG_PTR value = 0;
    USHORT typeIndex = FILE_TYPE_INDEX;
    ULONG access = READ_WRITE_ACCESS;
    DWORD fileType = FILE_TYPE_DISK;
    std::wstring path;
    bool hangs = false; // GetFileType blocks until released
};

struct FakeSystem
{
    DWORD openError = 0;
    std::uint64_t creationTicks = START_TICKS;
    std::vector<FakeHandle> handles;
    int snapshotCalls = 0;
    int createFileCalls = 0;
    std::atomic<int> duplicates{0};
    std::atomic<int> closes{0};
    std::atomic<int> processOpens{0};
    // Failures (#2039): each off unless set.
    bool timesFail = false;        // GetProcessTimes
    bool createFileFails = false;  // the NUL probe's CreateFile
    LONG snapshotStatus = 0;       // NtQuerySystemInformation's failure status
    ULONG snapshotNeeded = 0;      // a table that always needs this many bytes
    ULONG_PTR refuseDuplicate = 0; // a handle DuplicateHandle refuses
    ULONG_PTR throwOnName = 0;     // a handle whose name query throws
    std::wstring ntPath;           // the VOLUME_NAME_NT answer when the DOS one fails
};

FakeSystem& fake()
{
    static FakeSystem instance;
    return instance;
}

std::atomic<bool>& released()
{
    static std::atomic<bool> flag{false};
    return flag;
}

/// Set once a namer is inside a hanging fake: the read is waiting on the handle that never answers.
std::atomic<bool>& hangEntered()
{
    static std::atomic<bool> flag{false};
    return flag;
}

/// Polls @p done every 5 ms for up to 10 s. Only a bound against a hang: a passing test returns as
/// soon as @p done holds, and a loaded machine (#1797) gets far longer than it needs.
template<typename Predicate> [[nodiscard]] bool waitUntil(Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

HANDLE fakeProcessHandle()
{
    static int token = 0;
    return &token;
}

/// A duplicate's value: the source handle with a marker bit, so the fakes can find it again.
constexpr ULONG_PTR DUPLICATE_BIT = 0x10000000;

const FakeHandle* findDuplicate(HANDLE handle)
{
    const auto value = reinterpret_cast<ULONG_PTR>(handle) & ~DUPLICATE_BIT;
    const auto it = std::ranges::find(fake().handles, value, &FakeHandle::value);
    return it == fake().handles.end() ? nullptr : &*it;
}

LONG NTAPI fakeNtQuerySystemInformation(ULONG infoClass, PVOID buffer, ULONG bytes, PULONG needed)
{
    EXPECT_EQ(infoClass, SYSTEM_EXTENDED_HANDLE_INFORMATION);
    ++fake().snapshotCalls;
    if (fake().snapshotStatus != 0)
    {
        return fake().snapshotStatus;
    }
    if (fake().snapshotNeeded != 0)
    {
        *needed = fake().snapshotNeeded;
        return static_cast<LONG>(0xC0000004L);
    }
    std::vector<SystemHandleEntry> entries;
    for (const FakeHandle& handle : fake().handles)
    {
        entries.push_back({.object = nullptr,
                           .uniqueProcessId = handle.pid,
                           .handleValue = handle.value,
                           .grantedAccess = handle.access,
                           .creatorBackTraceIndex = 0,
                           .objectTypeIndex = handle.typeIndex,
                           .handleAttributes = 0,
                           .reserved = 0});
    }
    const std::size_t total = (2 * sizeof(ULONG_PTR)) + (entries.size() * sizeof(SystemHandleEntry));
    *needed = static_cast<ULONG>(total);
    if (bytes < total)
    {
        return static_cast<LONG>(0xC0000004L);
    }
    const std::array<ULONG_PTR, 2> header{entries.size(), 0};
    std::memcpy(buffer, header.data(), sizeof(header));
    std::memcpy(static_cast<std::byte*>(buffer) + sizeof(header), entries.data(), entries.size() * sizeof(SystemHandleEntry));
    return 0;
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    EXPECT_EQ(access, static_cast<DWORD>(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION));
    EXPECT_EQ(pid, static_cast<DWORD>(TARGET.pid));
    if (fake().openError != 0)
    {
        SetLastError(fake().openError);
        return nullptr;
    }
    ++fake().processOpens;
    return fakeProcessHandle();
}

BOOL WINAPI fakeCloseHandle(HANDLE /*handle*/)
{
    ++fake().closes;
    return TRUE;
}

BOOL WINAPI fakeGetProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    if (fake().timesFail)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    creation->dwLowDateTime = static_cast<DWORD>(fake().creationTicks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(fake().creationTicks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeDuplicateHandle(HANDLE source, HANDLE handle, HANDLE target, LPHANDLE out, DWORD /*access*/, BOOL inherit, DWORD options)
{
    EXPECT_EQ(source, fakeProcessHandle());
    EXPECT_EQ(target, GetCurrentProcess());
    EXPECT_EQ(inherit, FALSE);
    EXPECT_EQ(options, static_cast<DWORD>(DUPLICATE_SAME_ACCESS)); // never DUPLICATE_CLOSE_SOURCE
    if (reinterpret_cast<ULONG_PTR>(handle) == fake().refuseDuplicate)
    {
        return FALSE;
    }
    ++fake().duplicates;
    *out = reinterpret_cast<HANDLE>(reinterpret_cast<ULONG_PTR>(handle) | DUPLICATE_BIT); // NOLINT(performance-no-int-to-ptr)
    return TRUE;
}

DWORD WINAPI fakeGetFileType(HANDLE handle)
{
    const FakeHandle* found = findDuplicate(handle);
    if (found == nullptr)
    {
        return FILE_TYPE_UNKNOWN;
    }
    if (found->hangs)
    {
        hangEntered() = true;
    }
    while (found->hangs && !released())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return found->fileType;
}

/// A handle's DOS path, or fake().ntPath when the NT form is asked for (only after the DOS one fails).
DWORD WINAPI fakeGetFinalPathNameByHandle(HANDLE handle, LPWSTR buffer, DWORD size, DWORD flags)
{
    const FakeHandle* found = findDuplicate(handle);
    if (found != nullptr && found->value == fake().throwOnName)
    {
        throw std::bad_alloc();
    }
    std::wstring path;
    if ((flags & VOLUME_NAME_NT) != 0)
    {
        path = fake().ntPath;
    }
    else if (found != nullptr)
    {
        path = found->path;
    }
    if (path.empty())
    {
        return 0;
    }
    if (path.size() >= size)
    {
        return static_cast<DWORD>(path.size() + 1);
    }
    std::ranges::copy(path, buffer);
    buffer[path.size()] = L'\0';
    return static_cast<DWORD>(path.size());
}

HANDLE WINAPI fakeCreateFile(
    LPCWSTR name, DWORD /*access*/, DWORD /*share*/, LPSECURITY_ATTRIBUTES /*sa*/, DWORD /*disp*/, DWORD /*flags*/, HANDLE /*tmpl*/)
{
    EXPECT_STREQ(name, L"NUL");
    if (fake().createFileFails)
    {
        return INVALID_HANDLE_VALUE;
    }
    ++fake().createFileCalls;
    ++fake().processOpens;                       // closed like any other handle
    return reinterpret_cast<HANDLE>(NUL_HANDLE); // NOLINT(performance-no-int-to-ptr)
}

DWORD WINAPI fakeGetCurrentProcessId()
{
    return SELF_PID;
}

ProcessOpenFilesFunctions fakeApi()
{
    return {.ntQuerySystemInformation = &fakeNtQuerySystemInformation,
            .openProcess = &fakeOpenProcess,
            .closeHandle = &fakeCloseHandle,
            .getProcessTimes = &fakeGetProcessTimes,
            .duplicateHandle = &fakeDuplicateHandle,
            .getFileType = &fakeGetFileType,
            .getFinalPathNameByHandle = &fakeGetFinalPathNameByHandle,
            .createFile = &fakeCreateFile,
            .getCurrentProcessId = &fakeGetCurrentProcessId};
}

class WindowsProcessOpenFilesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        released() = false;
        hangEntered() = false;
        fake().openError = 0;
        fake().creationTicks = START_TICKS;
        fake().snapshotCalls = 0;
        fake().createFileCalls = 0;
        fake().duplicates = 0;
        fake().closes = 0;
        fake().processOpens = 0;
        fake().timesFail = false;
        fake().createFileFails = false;
        fake().snapshotStatus = 0;
        fake().snapshotNeeded = 0;
        fake().refuseDuplicate = 0;
        fake().throwOnName = 0;
        fake().ntPath.clear();
        fake().handles = {
            {.pid = SELF_PID,
             .value = NUL_HANDLE,
             .typeIndex = FILE_TYPE_INDEX,
             .access = 0,
             .fileType = FILE_TYPE_CHAR,
             .path = {},
             .hangs = false},
            {.pid = TARGET.pid,
             .value = 0x40,
             .typeIndex = FILE_TYPE_INDEX,
             .access = READ_WRITE_ACCESS,
             .fileType = FILE_TYPE_DISK,
             .path = L"\\\\?\\C:\\Data\\My File.txt",
             .hangs = false},
            {.pid = TARGET.pid,
             .value = 0x44,
             .typeIndex = 5,
             .access = 0x1F0003,
             .fileType = FILE_TYPE_UNKNOWN,
             .path = {},
             .hangs = false}, // an Event: not a file
            {.pid = TARGET.pid,
             .value = 0x48,
             .typeIndex = FILE_TYPE_INDEX,
             .access = READ_WRITE_ACCESS,
             .fileType = FILE_TYPE_PIPE,
             .path = {},
             .hangs = false},
            {.pid = TARGET.pid,
             .value = 0x4C,
             .typeIndex = FILE_TYPE_INDEX,
             .access = 0x00120189,
             .fileType = FILE_TYPE_DISK,
             .path = L"C:\\never",
             .hangs = false},
            {.pid = TARGET.pid,
             .value = 0x30,
             .typeIndex = FILE_TYPE_INDEX,
             .access = 0x00100001,
             .fileType = FILE_TYPE_DISK,
             .path = L"\\\\?\\UNC\\srv\\share\\a.log",
             .hangs = false},
            {.pid = 9999,
             .value = 0x40,
             .typeIndex = FILE_TYPE_INDEX,
             .access = READ_WRITE_ACCESS,
             .fileType = FILE_TYPE_DISK,
             .path = L"C:\\other",
             .hangs = false},
        };
    }

    void TearDown() override
    {
        released() = true;
        // Every handle closed once. A namer thread drops its share of the read (and the process handle
        // that goes with it) just after it finishes, so wait for it.
        EXPECT_TRUE(waitUntil([] { return fake().processOpens.load() + fake().duplicates.load() == fake().closes.load(); }));
    }

    /// Releases a hung fake and waits for @p reader's abandoned namers to finish.
    static void releaseAndDrain(WindowsProcessOpenFilesReader& reader)
    {
        released() = true;
        EXPECT_TRUE(waitUntil([&reader] { return reader.stuckNamerCount() == 0; }));
    }
};

TEST_F(WindowsProcessOpenFilesTest, ListsTheTargetsFileHandlesTypedAndNamed)
{
    WindowsProcessOpenFilesReader reader(fakeApi());
    EXPECT_TRUE(reader.hasOpenFiles());
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    EXPECT_TRUE(result.hexDescriptors);
    EXPECT_FALSE(result.namesIncomplete);
    ASSERT_EQ(result.files.size(), 4U); // not the Event, not another process's
    EXPECT_EQ(result.files[0].descriptor, 0x30U);
    EXPECT_EQ(result.files[0].path, "\\\\srv\\share\\a.log");
    EXPECT_EQ(result.files[1].descriptor, 0x40U);
    EXPECT_EQ(result.files[1].kind, OpenFileKind::File);
    EXPECT_EQ(result.files[1].path, "C:\\Data\\My File.txt");
    EXPECT_EQ(result.files[2].kind, OpenFileKind::Pipe);
    EXPECT_TRUE(result.files[2].path.empty());            // a pipe is never name-queried
    EXPECT_EQ(result.files[3].kind, OpenFileKind::Other); // a hang-prone mask: listed, never touched
    EXPECT_EQ(fake().duplicates.load(), 3);
    EXPECT_EQ(reader.handlesScanned(), 4U); // the progress the view shows while it runs
}

TEST_F(WindowsProcessOpenFilesTest, LearnsTheFileTypeOnce)
{
    WindowsProcessOpenFilesReader reader(fakeApi());
    static_cast<void>(reader.readOpenFiles(TARGET));
    static_cast<void>(reader.readOpenFiles(TARGET));
    EXPECT_EQ(fake().createFileCalls, 1);
    EXPECT_EQ(fake().snapshotCalls, 3); // the type probe's, then one per read
}

TEST_F(WindowsProcessOpenFilesTest, DeniedReusedAndUnknownTargetsAreNotRead)
{
    WindowsProcessOpenFilesReader reader(fakeApi());
    fake().openError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::PermissionDenied);
    fake().openError = ERROR_INVALID_PARAMETER;
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::ProcessExited);
    fake().openError = 0;
    fake().creationTicks = START_TICKS + 1;
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::ProcessExited);
    EXPECT_EQ(reader.readOpenFiles({.pid = TARGET.pid, .startTimeTicks = 0}).status, OpenFilesReadStatus::IdentityUnknown);
    EXPECT_EQ(fake().snapshotCalls, 0);
    EXPECT_EQ(fake().duplicates.load(), 0);
}

TEST_F(WindowsProcessOpenFilesTest, AHandleThatNeverAnswersIsAbandonedAndSkippedAfter)
{
    // The per-handle timeout is measured from the namer's last answer, so it also has to cover the
    // namer thread starting and naming 0x40 first. 50 ms did not on a loaded machine (#1797): 0x40
    // was given up on in its place, and the second read left it unnamed. 2 s is far beyond any
    // scheduling delay, and the read waits it out once, on the handle that hangs.
    constexpr std::chrono::milliseconds HANDLE_TIMEOUT{2000};
    fake().handles[3].hangs = true; // the pipe at 0x48
    WindowsProcessOpenFilesReader reader(fakeApi(), HANDLE_TIMEOUT);
    const OpenFilesReadResult first = reader.readOpenFiles(TARGET);
    ASSERT_EQ(first.status, OpenFilesReadStatus::Ok);
    EXPECT_TRUE(hangEntered()); // abandoned on the handle that hangs, not before reaching it
    EXPECT_TRUE(first.namesIncomplete);
    ASSERT_EQ(first.files.size(), 4U);                       // every handle still listed
    EXPECT_EQ(first.files[1].path, "C:\\Data\\My File.txt"); // named before the hang
    EXPECT_EQ(first.files[2].kind, OpenFileKind::Other);     // the hung pipe, never typed
    EXPECT_EQ(reader.stuckNamerCount(), 1U);

    // Later reads skip that handle and name the rest.
    const OpenFilesReadResult second = reader.readOpenFiles(TARGET);
    EXPECT_TRUE(second.namesIncomplete);
    ASSERT_EQ(second.files.size(), 4U);
    EXPECT_EQ(second.files[1].path, "C:\\Data\\My File.txt");
    EXPECT_EQ(second.files[2].kind, OpenFileKind::Other);
    releaseAndDrain(reader);
}

TEST_F(WindowsProcessOpenFilesTest, CapsTheCount)
{
    fake().handles.resize(1); // just the NUL probe
    for (std::size_t i = 0; i < MAX_OPEN_FILES + 5; ++i)
    {
        fake().handles.push_back({.pid = TARGET.pid,
                                  .value = 0x100000 + (i * 4),
                                  .typeIndex = FILE_TYPE_INDEX,
                                  .access = 0x00100000, // never touched: keeps the test fast
                                  .fileType = FILE_TYPE_DISK,
                                  .path = {},
                                  .hangs = false});
    }
    WindowsProcessOpenFilesReader reader(fakeApi());
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.files.size(), MAX_OPEN_FILES);
}

// A path longer than MAX_PATH is read again into a buffer that fits; a file without a DOS path
// gets its NT device path; one with neither, a handle that can't be duplicated and one whose name
// query throws are still listed, unnamed (#2039).
TEST_F(WindowsProcessOpenFilesTest, LongNtAndUnnamablePathsAreAllListed)
{
    const std::wstring longPath = L"C:\\" + std::wstring(300, L'a');
    fake().handles[1].path = L"\\\\?\\" + longPath; // 0x40
    fake().handles[4].access = READ_WRITE_ACCESS;   // 0x4C: queried now
    fake().handles[4].path.clear();                 // no DOS path
    fake().ntPath = L"\\Device\\HarddiskVolume3\\nt.txt";
    fake().refuseDuplicate = 0x48;
    WindowsProcessOpenFilesReader reader(fakeApi());
    OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    ASSERT_EQ(result.files.size(), 4U);
    EXPECT_EQ(result.files[1].path, "C:\\" + std::string(300, 'a'));
    EXPECT_EQ(result.files[2].kind, OpenFileKind::Other); // 0x48: not duplicated
    EXPECT_EQ(result.files[3].path, "\\Device\\HarddiskVolume3\\nt.txt");

    fake().ntPath.clear();
    fake().throwOnName = 0x40; // named first: the rest are listed unnamed
    result = reader.readOpenFiles(TARGET);
    ASSERT_EQ(result.files.size(), 4U);
    EXPECT_TRUE(std::ranges::all_of(result.files, [](const OpenFile& file) { return file.path.empty(); }));
}

TEST_F(WindowsProcessOpenFilesTest, FailedCallsFailTheRead)
{
    WindowsProcessOpenFilesReader reader(fakeApi());
    EXPECT_EQ(reader.readOpenFiles({.pid = 0, .startTimeTicks = START_TICKS}).status, OpenFilesReadStatus::ProcessExited);
    fake().openError = ERROR_GEN_FAILURE;
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    EXPECT_EQ(result.status, OpenFilesReadStatus::Failed);
    EXPECT_FALSE(result.detail.empty()); // the system's message for the error
    fake().openError = 0;
    fake().timesFail = true;
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::Failed);
    fake().timesFail = false;

    // The File type can't be learned: no NUL handle, then no handle table.
    fake().createFileFails = true;
    EXPECT_EQ(reader.readOpenFiles(TARGET).detail, "Could not identify file handles");
    fake().createFileFails = false;
    fake().snapshotStatus = static_cast<LONG>(0xC0000022L); // STATUS_ACCESS_DENIED
    EXPECT_EQ(reader.readOpenFiles(TARGET).detail, "Could not identify file handles");

    // Learned, then the read's own table fails, or outgrows the cap.
    fake().snapshotStatus = 0;
    ASSERT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::Ok);
    fake().snapshotStatus = static_cast<LONG>(0xC0000022L);
    EXPECT_EQ(reader.readOpenFiles(TARGET).detail, "NtQuerySystemInformation failed (0xC0000022)");
    fake().snapshotStatus = 0;
    fake().snapshotNeeded = 600U << 20U;
    EXPECT_EQ(reader.readOpenFiles(TARGET).detail, "The system handle table is too large to read");
}

// Once MAX_STUCK_NAMERS namers wait on handles that never answer, a read names nothing more: every
// handle is listed unnamed and none is touched.
TEST_F(WindowsProcessOpenFilesTest, TooManyStuckNamersListTheRestUnnamed)
{
    fake().handles.resize(1); // just the NUL probe
    for (ULONG_PTR i = 0; i < 24; ++i)
    {
        fake().handles.push_back({.pid = TARGET.pid,
                                  .value = 0x200 + (i * 4),
                                  .typeIndex = FILE_TYPE_INDEX,
                                  .access = READ_WRITE_ACCESS,
                                  .fileType = FILE_TYPE_DISK,
                                  .path = {},
                                  .hangs = true});
    }
    WindowsProcessOpenFilesReader reader(fakeApi(), std::chrono::milliseconds(100));
    // Each read gives up on the first handle not given up on before. A namer that had not started
    // by then never touches it and finishes, so read until enough are left waiting.
    for (int read = 0; read < 20 && reader.stuckNamerCount() < MAX_STUCK_NAMERS; ++read)
    {
        static_cast<void>(reader.readOpenFiles(TARGET));
    }
    ASSERT_EQ(reader.stuckNamerCount(), MAX_STUCK_NAMERS);
    const int duplicatesBefore = fake().duplicates.load();
    const OpenFilesReadResult result = reader.readOpenFiles(TARGET);
    EXPECT_TRUE(result.namesIncomplete);
    EXPECT_EQ(result.files.size(), 24U);
    EXPECT_EQ(fake().duplicates.load(), duplicatesBefore);
    releaseAndDrain(reader);
}

TEST(WindowsProcessOpenFilesSnapshotTest, ClampsACountLargerThanTheBuffer)
{
    std::vector<std::byte> buffer((2 * sizeof(ULONG_PTR)) + sizeof(SystemHandleEntry));
    const ULONG_PTR count = 1000; // claims far more than it holds
    std::memcpy(buffer.data(), &count, sizeof(count));
    SystemHandleEntry entry;
    entry.uniqueProcessId = 5;
    std::memcpy(buffer.data() + (2 * sizeof(ULONG_PTR)), &entry, sizeof(entry));
    EXPECT_EQ(handlesOfProcess(buffer, 5).size(), 1U);
    EXPECT_TRUE(handlesOfProcess(std::span(buffer).first(3), 5).empty());
}

TEST(WindowsProcessOpenFilesRealTest, ReadsThisProcess)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime), 0);
    const ProcessTarget self{.pid = static_cast<std::int32_t>(GetCurrentProcessId()),
                             .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime};
    std::array<wchar_t, MAX_PATH + 1> temp{};
    ASSERT_GT(GetTempPathW(MAX_PATH, temp.data()), 0U);
    const std::wstring path = std::wstring(temp.data()) + L"ts_open_files_probe.txt";
    auto* const file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);

    WindowsProcessOpenFilesReader reader;
    ASSERT_TRUE(reader.hasOpenFiles());
    const OpenFilesReadResult result = reader.readOpenFiles(self);
    CloseHandle(file);
    ASSERT_EQ(result.status, OpenFilesReadStatus::Ok);
    const auto found = std::ranges::find(result.files, reinterpret_cast<std::uint64_t>(file), &OpenFile::descriptor);
    ASSERT_NE(found, result.files.end());
    EXPECT_EQ(found->kind, OpenFileKind::File);
    EXPECT_TRUE(found->path.ends_with("ts_open_files_probe.txt")) << found->path;
}

} // namespace
} // namespace Platform::Windows
