/// @file test_WindowsProcessOpenFiles.cpp
/// @brief Platform::Windows::WindowsProcessOpenFilesReader (#183) against a fake handle table and fake
/// kernel32 calls: only the target's File handles listed, typed and named (disks only), the File type
/// learned once, hang-prone access masks never touched, a handle that never answers abandoned under the
/// timeout and skipped after, the count cap, a denied open, a reused PID, every handle duplicated with
/// DUPLICATE_SAME_ACCESS and closed. Plus one read of this test process through the real calls.

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
    while (found->hangs && !released())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return found->fileType;
}

DWORD WINAPI fakeGetFinalPathNameByHandle(HANDLE handle, LPWSTR buffer, DWORD size, DWORD flags)
{
    EXPECT_EQ(flags & VOLUME_NAME_NT, 0U); // the DOS form answers first in these fakes
    const FakeHandle* found = findDuplicate(handle);
    if (found == nullptr || found->path.empty())
    {
        return 0;
    }
    if (found->path.size() >= size)
    {
        return static_cast<DWORD>(found->path.size() + 1);
    }
    std::ranges::copy(found->path, buffer);
    buffer[found->path.size()] = L'\0';
    return static_cast<DWORD>(found->path.size());
}

HANDLE WINAPI fakeCreateFile(
    LPCWSTR name, DWORD /*access*/, DWORD /*share*/, LPSECURITY_ATTRIBUTES /*sa*/, DWORD /*disp*/, DWORD /*flags*/, HANDLE /*tmpl*/)
{
    EXPECT_STREQ(name, L"NUL");
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
        fake().openError = 0;
        fake().creationTicks = START_TICKS;
        fake().snapshotCalls = 0;
        fake().createFileCalls = 0;
        fake().duplicates = 0;
        fake().closes = 0;
        fake().processOpens = 0;
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
        // that goes with it) just after it finishes, so give it a moment.
        const auto balanced = []
        {
            return fake().processOpens.load() + fake().duplicates.load() == fake().closes.load();
        };
        for (int i = 0; i < 200 && !balanced(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(balanced());
    }

    /// Releases a hung fake and waits for @p reader's abandoned namers to finish.
    static void releaseAndDrain(WindowsProcessOpenFilesReader& reader)
    {
        released() = true;
        for (int i = 0; i < 400 && reader.stuckNamerCount() > 0; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(reader.stuckNamerCount(), 0U);
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
    fake().handles[3].hangs = true; // the pipe at 0x48
    WindowsProcessOpenFilesReader reader(fakeApi(), std::chrono::milliseconds(50));
    const OpenFilesReadResult first = reader.readOpenFiles(TARGET);
    ASSERT_EQ(first.status, OpenFilesReadStatus::Ok);
    EXPECT_TRUE(first.namesIncomplete);
    EXPECT_EQ(first.files.size(), 4U); // every handle still listed
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
