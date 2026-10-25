/// @file test_WindowsProcessEnvironmentReader.cpp
/// @brief Platform::Windows::WindowsProcessEnvironmentReader (#2569) against a fake process whose PEB,
/// process parameters and environment block are laid out in a byte buffer a fake ReadProcessMemory
/// serves: the NAME=value entries (a "=C:=C:\" drive entry, a non-ASCII value, an empty value, a
/// control character, an entry without '='), nothing past the terminator, a denied open, a reused PID,
/// an unknown start time, the block-size and variable-count caps, the scan to the double NUL when
/// EnvironmentSize is 0 (its terminator straddling a page), a 32-bit (WOW64) target refused with its
/// reason, failures of each step with theirs, a process that exits mid-read, and the handle closed
/// exactly once. Plus one read of this test process through the real calls.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/Windows/WindowsProcessEnvironmentReader.h"

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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = START_TICKS};

// The fake process's memory, as the reader addresses it: its PEB at REMOTE_BASE, its process parameters
// a page on, its environment block two pages on, all in one byte buffer. The field offsets are the
// 64-bit PEB's and RTL_USER_PROCESS_PARAMETERS's, spelled out here apart from the reader's own.
static_assert(sizeof(void*) == 8, "the fake PEB below is laid out for the 64-bit reader");
constexpr std::uintptr_t REMOTE_BASE = 0x00007FF6'1000'0000ULL;
constexpr std::size_t PAGE = 4096;
constexpr std::size_t PARAMETERS_OFFSET = PAGE;
constexpr std::size_t BLOCK_OFFSET = 2 * PAGE;
constexpr std::size_t PEB_PROCESS_PARAMETERS = 0x20;
constexpr std::size_t PARAMETERS_ENVIRONMENT = 0x80;
constexpr std::size_t PARAMETERS_ENVIRONMENT_SIZE = 0x3F0;

/// The answers the fakes give and what they saw; each test starts from a fresh one.
struct FakeProcess
{
    DWORD openError = 0; // non-zero: OpenProcess fails with it
    std::uint64_t creationTicks = START_TICKS;
    DWORD exitCode = STILL_ACTIVE;
    USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN; // IsWow64Process2's process machine for the target
    USHORT selfMachine = IMAGE_FILE_MACHINE_UNKNOWN;   // ... and for this process (AMD64: an x64 TaskSmack emulated on ARM64)
    LONG queryStatus = 0;                              // NtQueryInformationProcess's NTSTATUS
    std::uintptr_t pebAddress = REMOTE_BASE;           // what it reports; 0 for no PEB
    DWORD readError = 0;                               // non-zero: every ReadProcessMemory fails with it
    std::vector<std::byte> memory;                     // the target's memory from REMOTE_BASE; a read past its end fails
    int opens = 0;
    int closes = 0;
    int reads = 0;
    std::size_t bytesRequested = 0;
    std::size_t largestRead = 0;
};

FakeProcess& fake()
{
    static FakeProcess instance;
    return instance;
}

HANDLE fakeProcessHandle()
{
    static int token = 0;
    return &token;
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    EXPECT_EQ(access, static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ));
    EXPECT_EQ(pid, static_cast<DWORD>(TARGET.pid));
    if (fake().openError != 0)
    {
        SetLastError(fake().openError);
        return nullptr;
    }
    ++fake().opens;
    return fakeProcessHandle();
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_EQ(handle, fakeProcessHandle());
    ++fake().closes;
    return TRUE;
}

BOOL WINAPI fakeGetProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    creation->dwLowDateTime = static_cast<DWORD>(fake().creationTicks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(fake().creationTicks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeGetExitCodeProcess(HANDLE /*process*/, LPDWORD code)
{
    *code = fake().exitCode;
    return TRUE;
}

/// This process's and the target's machines are what the test says (native by default).
BOOL WINAPI fakeIsWow64Process2(HANDLE process, USHORT* machine, USHORT* native)
{
    *native = IMAGE_FILE_MACHINE_AMD64;
    *machine = (process == GetCurrentProcess()) ? fake().selfMachine : fake().targetMachine;
    return TRUE;
}

LONG NTAPI fakeNtQueryInformationProcess(HANDLE process, ULONG informationClass, PVOID information, ULONG length, PULONG written)
{
    EXPECT_EQ(process, fakeProcessHandle());
    EXPECT_EQ(informationClass, 0U); // ProcessBasicInformation
    EXPECT_EQ(length, static_cast<ULONG>(sizeof(PROCESS_BASIC_INFORMATION)));
    if (fake().queryStatus < 0)
    {
        return fake().queryStatus;
    }
    PROCESS_BASIC_INFORMATION basic{};
    basic.PebBaseAddress = reinterpret_cast<PPEB>(fake().pebAddress); // NOLINT(performance-no-int-to-ptr) - a fake address
    basic.UniqueProcessId = static_cast<ULONG_PTR>(TARGET.pid);
    std::memcpy(information, &basic, sizeof(basic));
    *written = sizeof(basic);
    return 0;
}

BOOL WINAPI fakeReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read)
{
    EXPECT_EQ(process, fakeProcessHandle());
    ++fake().reads;
    fake().bytesRequested += size;
    fake().largestRead = std::max<std::size_t>(fake().largestRead, size);
    *read = 0;
    if (fake().readError != 0)
    {
        SetLastError(fake().readError);
        return FALSE;
    }
    const auto remote = reinterpret_cast<std::uintptr_t>(address);
    if (remote < REMOTE_BASE || remote - REMOTE_BASE + size > fake().memory.size())
    {
        SetLastError(ERROR_PARTIAL_COPY); // past the fake's mapped memory
        return FALSE;
    }
    std::memcpy(buffer, fake().memory.data() + (remote - REMOTE_BASE), size);
    *read = size;
    return TRUE;
}

ProcessEnvironmentFunctions fakeApi()
{
    return {.openProcess = &fakeOpenProcess,
            .closeHandle = &fakeCloseHandle,
            .getProcessTimes = &fakeGetProcessTimes,
            .getExitCodeProcess = &fakeGetExitCodeProcess,
            .isWow64Process2 = &fakeIsWow64Process2,
            .ntQueryInformationProcess = &fakeNtQueryInformationProcess,
            .readProcessMemory = &fakeReadProcessMemory};
}

/// Writes @p value at @p offset of the fake's memory.
void poke(std::size_t offset, std::uintptr_t value)
{
    std::memcpy(fake().memory.data() + offset, &value, sizeof(value));
}

/// Lays the fake process out: a PEB pointing at its process parameters, those at @p block (UTF-16,
/// with whatever terminator the test gives it) with @p environmentSize as EnvironmentSize, then
/// @p slack more bytes of 0xCC past the block. The mapped memory ends at a page boundary, as real
/// memory does: whatever is between the block's end and it reads as 0xCC.
void layOut(std::wstring_view block, std::uintptr_t environmentSize, std::size_t slack = 0)
{
    const std::size_t blockBytes = block.size() * sizeof(wchar_t);
    const std::size_t used = BLOCK_OFFSET + blockBytes + slack;
    fake().memory.assign(((used + PAGE - 1) / PAGE) * PAGE, std::byte{0xCC});
    std::fill_n(fake().memory.begin(), BLOCK_OFFSET, std::byte{0});
    poke(PEB_PROCESS_PARAMETERS, REMOTE_BASE + PARAMETERS_OFFSET);
    poke(PARAMETERS_OFFSET + PARAMETERS_ENVIRONMENT, REMOTE_BASE + BLOCK_OFFSET);
    poke(PARAMETERS_OFFSET + PARAMETERS_ENVIRONMENT_SIZE, environmentSize);
    if (blockBytes > 0)
    {
        std::memcpy(fake().memory.data() + BLOCK_OFFSET, block.data(), blockBytes);
    }
}

/// @p entries as an environment block: each ended by a NUL, then the terminating NUL.
[[nodiscard]] std::wstring joinBlock(const std::vector<std::wstring>& entries)
{
    std::wstring block;
    for (const std::wstring& entry : entries)
    {
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

/// The value of @p name in @p result, or nullopt if no entry has that name.
[[nodiscard]] std::optional<std::string> valueOf(const EnvironmentReadResult& result, std::string_view name)
{
    const auto it = std::ranges::find(result.variables, name, &EnvironmentVariable::name);
    return (it == result.variables.end()) ? std::nullopt : std::optional(it->value);
}

class WindowsProcessEnvironmentReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
    }

    void TearDown() override
    {
        EXPECT_EQ(fake().opens, fake().closes); // every handle opened was closed
    }
};

TEST_F(WindowsProcessEnvironmentReaderTest, ReadsEveryVariableFromTheBlock)
{
    // A drive's working directory entry, a path, an empty value, non-ASCII (BMP and a surrogate pair),
    // a control character, and an entry without '='; then the terminator, then slack the size covers.
    const std::wstring block = joinBlock({L"=C:=C:\\Users\\me",
                                          L"PATH=C:\\Windows;C:\\Windows\\System32",
                                          L"EMPTY=",
                                          L"UNICODE=caf\u00E9 \u00DF \U0001F600",
                                          L"TAB=a\tb",
                                          L"NOEQUALS"});
    layOut(block, (block.size() * sizeof(wchar_t)) + 64, 64);

    WindowsProcessEnvironmentReader reader(fakeApi());
    EXPECT_TRUE(reader.hasEnvironment());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_FALSE(result.truncated);
    EXPECT_TRUE(result.detail.empty());
    ASSERT_EQ(result.variables.size(), 6U);
    EXPECT_EQ(result.variables[0].name, "=C:"); // a leading '=' stays in the name
    EXPECT_EQ(result.variables[0].value, "C:\\Users\\me");
    EXPECT_EQ(result.variables[1].name, "PATH");
    EXPECT_EQ(result.variables[1].value, "C:\\Windows;C:\\Windows\\System32");
    EXPECT_EQ(valueOf(result, "EMPTY"), std::optional<std::string>(""));
    EXPECT_EQ(valueOf(result, "UNICODE"), std::optional<std::string>("caf\xC3\xA9 \xC3\x9F \xF0\x9F\x98\x80"));
    EXPECT_EQ(valueOf(result, "TAB"), std::optional<std::string>("a\\tb")); // escaped for display
    EXPECT_EQ(valueOf(result, "NOEQUALS"), std::optional<std::string>(""));
    EXPECT_EQ(fake().reads, 4); // the three fields, then the block, which its first page holds whole
}

TEST_F(WindowsProcessEnvironmentReaderTest, AnEmptyBlockAndANullBlockAreOkWithNoVariables)
{
    layOut(std::wstring(2, L'\0'), 4); // an empty block is its terminator alone
    WindowsProcessEnvironmentReader reader(fakeApi());
    EnvironmentReadResult result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Ok);
    EXPECT_TRUE(result.variables.empty());

    poke(PARAMETERS_OFFSET + PARAMETERS_ENVIRONMENT, 0); // no block at all
    result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Ok);
    EXPECT_TRUE(result.variables.empty());
}

TEST_F(WindowsProcessEnvironmentReaderTest, DeniedOpenIsPermissionDeniedAndAVanishedPidIsProcessExited)
{
    layOut(joinBlock({L"A=1"}), 10);
    fake().openError = ERROR_ACCESS_DENIED;
    WindowsProcessEnvironmentReader reader(fakeApi());
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::PermissionDenied);

    fake().openError = ERROR_INVALID_PARAMETER; // no such PID
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::ProcessExited);
    EXPECT_EQ(fake().reads, 0);
}

TEST_F(WindowsProcessEnvironmentReaderTest, ReusedPidAndUnknownIdentityAreNotRead)
{
    layOut(joinBlock({L"A=1"}), 10);
    fake().creationTicks = START_TICKS + 1; // the PID now names another process
    WindowsProcessEnvironmentReader reader(fakeApi());
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::ProcessExited);
    EXPECT_EQ(fake().reads, 0);

    fake().creationTicks = START_TICKS;
    EXPECT_EQ(reader.readEnvironment({.pid = TARGET.pid, .startTimeTicks = 0}).status, EnvironmentReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readEnvironment({.pid = 0, .startTimeTicks = START_TICKS}).status, EnvironmentReadStatus::ProcessExited);
    EXPECT_EQ(fake().reads, 0);

    fake().exitCode = 0; // the target, exited, its object still held open (#2206)
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::ProcessExited);
    EXPECT_EQ(fake().reads, 0);
}

TEST_F(WindowsProcessEnvironmentReaderTest, AWow64TargetIsFailedWithItsReason)
{
    layOut(joinBlock({L"A=1"}), 10);
    fake().targetMachine = IMAGE_FILE_MACHINE_I386;
    WindowsProcessEnvironmentReader reader(fakeApi());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Failed);
    EXPECT_TRUE(result.detail.contains("32-bit")) << result.detail;
    EXPECT_TRUE(result.variables.empty());
    EXPECT_EQ(fake().reads, 0); // nothing of its memory is read

    fake().targetMachine = IMAGE_FILE_MACHINE_ARMNT; // ARM32 under WOW64 on ARM64, the same
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::Failed);

    fake().targetMachine = IMAGE_FILE_MACHINE_I386;
    fake().selfMachine = IMAGE_FILE_MACHINE_AMD64; // an x64 TaskSmack emulated on ARM64 is 64-bit all the same
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::Failed);
}

TEST_F(WindowsProcessEnvironmentReaderTest, AnEmulatedX64TargetOnArm64IsReadLikeANativeOne)
{
    // ARM64 Windows runs x64 processes under its WOW64 infrastructure, and IsWow64Process2 may report
    // AMD64 for them: still a 64-bit PEB, so the native (UNKNOWN) ARM64 TaskSmack reads it.
    layOut(joinBlock({L"A=1", L"B=2"}), 20);
    fake().targetMachine = IMAGE_FILE_MACHINE_AMD64;
    WindowsProcessEnvironmentReader reader(fakeApi());
    EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_EQ(result.variables.size(), 2U);

    // And the other way round: an x64 TaskSmack reported as AMD64 reads a native ARM64 (UNKNOWN) process.
    fake().targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    fake().selfMachine = IMAGE_FILE_MACHINE_AMD64;
    result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_EQ(result.variables.size(), 2U);
}

TEST_F(WindowsProcessEnvironmentReaderTest, BlockSizeCapIsRespected)
{
    // 600 entries of 1024 characters (2 KiB each): 1.2 MiB, EnvironmentSize telling the truth.
    std::wstring block;
    for (int i = 0; i < 600; ++i)
    {
        std::wstring entry = L"V" + std::to_wstring(i) + L"=";
        entry.resize(1023, L'x');
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    layOut(block, block.size() * sizeof(wchar_t));

    WindowsProcessEnvironmentReader reader(fakeApi());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.variables.size(), ENVIRONMENT_BLOCK_READ_CAP_BYTES / 2048); // the entries the first MiB holds whole
    EXPECT_EQ(result.variables.front().name, "V0");
    EXPECT_LE(fake().bytesRequested, ENVIRONMENT_BLOCK_READ_CAP_BYTES + (3 * sizeof(std::uintptr_t)));
    EXPECT_LE(fake().largestRead, PAGE);
}

TEST_F(WindowsProcessEnvironmentReaderTest, ASizeOverstatingTheBlockStopsAtItsTerminator)
{
    layOut(joinBlock({L"A=1", L"B=2"}), ENVIRONMENT_BLOCK_READ_CAP_BYTES); // claims a MiB; the fake maps one page of it
    WindowsProcessEnvironmentReader reader(fakeApi());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_FALSE(result.truncated);
    ASSERT_EQ(result.variables.size(), 2U);
    EXPECT_EQ(result.variables[1].name, "B");
}

TEST_F(WindowsProcessEnvironmentReaderTest, VariableCountCapIsRespected)
{
    std::vector<std::wstring> entries;
    entries.reserve(ENVIRONMENT_VARIABLE_CAP + 10);
    for (std::size_t i = 0; i < ENVIRONMENT_VARIABLE_CAP + 10; ++i)
    {
        entries.push_back(L"N" + std::to_wstring(i) + L"=v");
    }
    const std::wstring block = joinBlock(entries);
    layOut(block, block.size() * sizeof(wchar_t));
    WindowsProcessEnvironmentReader reader(fakeApi());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.variables.size(), ENVIRONMENT_VARIABLE_CAP);
}

TEST_F(WindowsProcessEnvironmentReaderTest, ZeroSizeScansToTheDoubleNulAcrossAPageBoundary)
{
    // EnvironmentSize 0 (as before Windows 8.1): the block ends at its double NUL, here with the first
    // NUL (the long entry's) as the last character of the block's first page and the second (the
    // terminator's own) as the first character of the next page.
    constexpr std::size_t PAGE_CHARS = PAGE / sizeof(wchar_t);
    std::wstring longEntry = L"LONG=";
    longEntry.resize(PAGE_CHARS - 5, L'y'); // "A=1" + NUL (4) + this + NUL fill the page: 4 + 2043 + 1
    std::wstring block = joinBlock({L"A=1", longEntry});
    ASSERT_EQ(block.size(), PAGE_CHARS + 1);
    block += L"GARBAGE=after the terminator";
    block += L'\0';
    layOut(block, 0, 2 * PAGE);

    WindowsProcessEnvironmentReader reader(fakeApi());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_FALSE(result.truncated);
    ASSERT_EQ(result.variables.size(), 2U);
    EXPECT_EQ(result.variables[0].name, "A");
    EXPECT_EQ(result.variables[1].name, "LONG");
    EXPECT_EQ(result.variables[1].value.size(), PAGE_CHARS - 10); // the y's, whole
    EXPECT_FALSE(valueOf(result, "GARBAGE").has_value());
    EXPECT_EQ(fake().reads, 5); // three fields, two pages
}

TEST_F(WindowsProcessEnvironmentReaderTest, ZeroSizeWithNoTerminatorBeforeUnmappedMemoryKeepsWhatWasReadAsTruncated)
{
    // A page filled by the block to its last character, with no terminator; the next page is unmapped.
    std::wstring block = L"A=1";
    block += L'\0';
    block += L"B=";
    block.resize(PAGE / sizeof(wchar_t), L'x');
    layOut(block, 0);
    WindowsProcessEnvironmentReader reader(fakeApi());
    const EnvironmentReadResult result = reader.readEnvironment(TARGET);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_TRUE(result.truncated);
    ASSERT_EQ(result.variables.size(), 2U);
    EXPECT_EQ(result.variables[1].name, "B");
    EXPECT_EQ(result.variables[1].value.size(), (PAGE / sizeof(wchar_t)) - 6);
    EXPECT_EQ(fake().reads, 5); // three fields, the page, then the next, which fails
}

TEST_F(WindowsProcessEnvironmentReaderTest, EachStepThatFailsSaysSo)
{
    layOut(joinBlock({L"A=1"}), 10);
    WindowsProcessEnvironmentReader reader(fakeApi());

    fake().queryStatus = static_cast<LONG>(0xC0000005); // STATUS_ACCESS_VIOLATION, say
    EnvironmentReadResult result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Failed);
    EXPECT_TRUE(result.detail.contains("NTSTATUS 0xC0000005")) << result.detail;

    fake().queryStatus = static_cast<LONG>(0xC0000022); // STATUS_ACCESS_DENIED
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::PermissionDenied);
    fake().queryStatus = static_cast<LONG>(0xC000010A); // STATUS_PROCESS_IS_TERMINATING
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::ProcessExited);
    fake().queryStatus = 0;

    fake().pebAddress = 0; // a minimal process
    result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Failed);
    EXPECT_TRUE(result.detail.contains("PEB")) << result.detail;
    fake().pebAddress = REMOTE_BASE;

    poke(PEB_PROCESS_PARAMETERS, 0); // still starting
    result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Failed);
    EXPECT_TRUE(result.detail.contains("process parameters")) << result.detail;
    poke(PEB_PROCESS_PARAMETERS, REMOTE_BASE + PARAMETERS_OFFSET);

    fake().readError = ERROR_NOACCESS; // the memory can't be read
    result = reader.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Failed);
    EXPECT_FALSE(result.detail.empty());

    fake().readError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::PermissionDenied);

    fake().exitCode = 0; // exited between the identity check and the read
    EXPECT_EQ(reader.readEnvironment(TARGET).status, EnvironmentReadStatus::ProcessExited);

    fake().readError = 0;
    fake().exitCode = STILL_ACTIVE;
    ProcessEnvironmentFunctions withoutNtdll = fakeApi();
    withoutNtdll.ntQueryInformationProcess = nullptr;
    WindowsProcessEnvironmentReader noQuery(withoutNtdll);
    result = noQuery.readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Failed);
    EXPECT_TRUE(result.detail.contains("NtQueryInformationProcess")) << result.detail;
}

/// The real calls, on this process: a variable set just before the read shows, because the block read
/// is the process's live one (SetEnvironmentVariableW writes into it).
TEST(WindowsProcessEnvironmentReaderRealTest, ReadsThisProcessesOwnEnvironment)
{
    constexpr const wchar_t* NAME = L"TASKSMACK_TEST_ENVIRONMENT_READER_2569";
    ASSERT_NE(SetEnvironmentVariableW(NAME, L"caf\u00E9 \U0001F600 value"), 0);

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    ASSERT_NE(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime), 0);
    const ProcessTarget self{.pid = static_cast<std::int32_t>(GetCurrentProcessId()),
                             .startTimeTicks = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime};
    WindowsProcessEnvironmentReader reader;
    EXPECT_TRUE(reader.hasEnvironment());
    const EnvironmentReadResult result = reader.readEnvironment(self);
    ASSERT_EQ(result.status, EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(valueOf(result, "TASKSMACK_TEST_ENVIRONMENT_READER_2569"), std::optional<std::string>("caf\xC3\xA9 \xF0\x9F\x98\x80 value"));
    EXPECT_TRUE(valueOf(result, "SystemRoot").has_value() || valueOf(result, "SYSTEMROOT").has_value()); // every Windows process has it
    for (const EnvironmentVariable& variable : result.variables)
    {
        EXPECT_FALSE(variable.name.empty());
    }

    // Gone again once unset: the block is the live one, not a copy from when the process started.
    ASSERT_NE(SetEnvironmentVariableW(NAME, nullptr), 0);
    EXPECT_FALSE(valueOf(reader.readEnvironment(self), "TASKSMACK_TEST_ENVIRONMENT_READER_2569").has_value());
}

} // namespace
} // namespace Platform::Windows
