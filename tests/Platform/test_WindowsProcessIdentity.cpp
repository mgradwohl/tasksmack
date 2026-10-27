/// @file test_WindowsProcessIdentity.cpp
/// @brief Platform::Windows::openProcessVerified() (#2149), the open-and-check-identity step every
/// Windows action and Process Details reader shares, against fake OpenProcess / GetProcessTimes /
/// GetExitCodeProcess / CloseHandle: an invalid PID, an unknown start time, a denied open, a
/// vanished process, another open error, a failed GetProcessTimes, a reused PID, an exited process
/// still held open (#2206) and a match, each with the access asked for, the Win32 error kept and the
/// handle closed exactly once. Then how a reader reports each refusal (toReadFailure), and checks of
/// this test process, and of an exited child it holds open, through the real calls.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessIdentity.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

namespace Platform::Windows
{
namespace
{

constexpr std::uint64_t START_TICKS = 0x01DB'0000'1234'5678ULL;
constexpr std::int32_t PID = 4242;

/// The answers the fakes give and what they saw; each test starts from a fresh one.
struct FakeKernel
{
    DWORD openError = 0;  // non-zero: OpenProcess fails with it
    DWORD timesError = 0; // non-zero: GetProcessTimes fails with it
    std::uint64_t creationTicks = START_TICKS;
    DWORD exitCode = STILL_ACTIVE; // what GetExitCodeProcess reports
    DWORD exitCodeError = 0;       // non-zero: GetExitCodeProcess fails with it
    DWORD accessAsked = 0;
    DWORD pidAsked = 0;
    int opens = 0;
    int timesCalls = 0;
    int exitCodeCalls = 0;
    int closes = 0;
};

FakeKernel& fake()
{
    static FakeKernel instance;
    return instance;
}

HANDLE fakeProcessHandle()
{
    static int token = 0;
    return &token;
}

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
{
    ++fake().opens;
    fake().accessAsked = access;
    fake().pidAsked = pid;
    if (fake().openError != 0)
    {
        SetLastError(fake().openError);
        return nullptr;
    }
    return fakeProcessHandle();
}

BOOL WINAPI fakeGetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
{
    EXPECT_EQ(process, fakeProcessHandle());
    ++fake().timesCalls;
    if (fake().timesError != 0)
    {
        SetLastError(fake().timesError);
        return FALSE;
    }
    creation->dwLowDateTime = static_cast<DWORD>(fake().creationTicks & 0xFFFFFFFFU);
    creation->dwHighDateTime = static_cast<DWORD>(fake().creationTicks >> 32U);
    return TRUE;
}

BOOL WINAPI fakeGetExitCodeProcess(HANDLE process, LPDWORD code)
{
    EXPECT_EQ(process, fakeProcessHandle());
    ++fake().exitCodeCalls;
    if (fake().exitCodeError != 0)
    {
        SetLastError(fake().exitCodeError);
        return FALSE;
    }
    *code = fake().exitCode;
    return TRUE;
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_EQ(handle, fakeProcessHandle());
    ++fake().closes;
    return TRUE;
}

constexpr ProcessIdentityFunctions FAKES{
    .openProcess = &fakeOpenProcess,
    .getProcessTimes = &fakeGetProcessTimes,
    .closeHandle = &fakeCloseHandle,
    .getExitCodeProcess = &fakeGetExitCodeProcess,
};

/// One refusal openProcessVerified() must make, and what must have happened on the way.
struct RefusalCase
{
    const char* name;
    ProcessTarget target;
    DWORD openError = 0;
    DWORD timesError = 0;
    std::uint64_t creationTicks = START_TICKS;
    DWORD exitCode = STILL_ACTIVE;
    IdentityFailureKind kind = IdentityFailureKind::OpenFailed;
    DWORD win32Error = 0;
    bool opened = false; // OpenProcess was called and returned a handle, which must be closed once
    bool attemptedOpen = false;

    friend std::ostream& operator<<(std::ostream& out, const RefusalCase& c)
    {
        return out << c.name;
    }
};

class OpenProcessVerifiedRefusal : public ::testing::TestWithParam<RefusalCase>
{
  protected:
    void SetUp() override
    {
        fake() = FakeKernel{};
    }
};

TEST_P(OpenProcessVerifiedRefusal, RefusesWithTheKindAndErrorAndClosesWhatItOpened)
{
    const RefusalCase& c = GetParam();
    fake().openError = c.openError;
    fake().timesError = c.timesError;
    fake().creationTicks = c.creationTicks;
    fake().exitCode = c.exitCode;

    {
        const auto result = openProcessVerified(FAKES, c.target, PROCESS_VM_READ);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, c.kind);
        EXPECT_EQ(result.error().win32Error, c.win32Error);
    }
    EXPECT_EQ(fake().opens, c.attemptedOpen ? 1 : 0);
    EXPECT_EQ(fake().closes, c.opened ? 1 : 0); // a handle opened is closed before the refusal returns
    EXPECT_EQ(fake().timesCalls, c.opened ? 1 : 0);
}

INSTANTIATE_TEST_SUITE_P(
    WindowsProcessIdentity,
    OpenProcessVerifiedRefusal,
    ::testing::Values(
        RefusalCase{.name = "ZeroPid", .target = {.pid = 0, .startTimeTicks = START_TICKS}, .kind = IdentityFailureKind::InvalidPid},
        RefusalCase{.name = "NegativePid", .target = {.pid = -1, .startTimeTicks = START_TICKS}, .kind = IdentityFailureKind::InvalidPid},
        // Idle and System report no start time: refused, never opened by PID alone.
        RefusalCase{.name = "UnknownStartTime", .target = {.pid = PID, .startTimeTicks = 0}, .kind = IdentityFailureKind::IdentityUnknown},
        RefusalCase{.name = "AccessDenied",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .openError = ERROR_ACCESS_DENIED,
                    .kind = IdentityFailureKind::AccessDenied,
                    .win32Error = ERROR_ACCESS_DENIED,
                    .attemptedOpen = true},
        RefusalCase{.name = "ProcessGone",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .openError = ERROR_INVALID_PARAMETER,
                    .kind = IdentityFailureKind::ProcessGone,
                    .win32Error = ERROR_INVALID_PARAMETER,
                    .attemptedOpen = true},
        RefusalCase{.name = "OtherOpenError",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .openError = ERROR_NOT_ENOUGH_MEMORY,
                    .kind = IdentityFailureKind::OpenFailed,
                    .win32Error = ERROR_NOT_ENOUGH_MEMORY,
                    .attemptedOpen = true},
        RefusalCase{.name = "GetProcessTimesFails",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .timesError = ERROR_GEN_FAILURE,
                    .kind = IdentityFailureKind::TimesFailed,
                    .win32Error = ERROR_GEN_FAILURE,
                    .opened = true,
                    .attemptedOpen = true},
        // The PID now names a process that started at another time: the target exited and its PID was reused.
        RefusalCase{.name = "ReusedPid",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .creationTicks = START_TICKS + 1,
                    .kind = IdentityFailureKind::Mismatch,
                    .opened = true,
                    .attemptedOpen = true},
        // Only the high half differs: both FILETIME halves are compared.
        RefusalCase{.name = "ReusedPidHighHalf",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .creationTicks = START_TICKS ^ (1ULL << 40U),
                    .kind = IdentityFailureKind::Mismatch,
                    .opened = true,
                    .attemptedOpen = true},
        // The target itself, exited but still held open by someone: its PID and start time still match (#2206).
        RefusalCase{.name = "ExitedButHeldOpen",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .exitCode = 1,
                    .kind = IdentityFailureKind::Exited,
                    .opened = true,
                    .attemptedOpen = true},
        RefusalCase{.name = "ExitedWithCodeZero",
                    .target = {.pid = PID, .startTimeTicks = START_TICKS},
                    .exitCode = 0,
                    .kind = IdentityFailureKind::Exited,
                    .opened = true,
                    .attemptedOpen = true}),
    [](const ::testing::TestParamInfo<RefusalCase>& info) { return std::string(info.param.name); });

TEST(WindowsProcessIdentity, MatchReturnsTheOpenHandleWithTheAccessAskedForAndQueryAccess)
{
    fake() = FakeKernel{};
    {
        auto result = openProcessVerified(FAKES, {.pid = PID, .startTimeTicks = START_TICKS}, PROCESS_DUP_HANDLE);

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->get(), fakeProcessHandle());
        EXPECT_EQ(fake().pidAsked, static_cast<DWORD>(PID));
        // GetProcessTimes needs PROCESS_QUERY_LIMITED_INFORMATION, so it is always added.
        EXPECT_EQ(fake().accessAsked, static_cast<DWORD>(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION));
        EXPECT_EQ(fake().exitCodeCalls, 1); // STILL_ACTIVE: running
        EXPECT_EQ(fake().closes, 0);        // the caller owns it now

        const InjectableHandle owned = std::move(*result);
        EXPECT_EQ(fake().closes, 0);
    }
    EXPECT_EQ(fake().closes, 1); // closed once, through the injected CloseHandle
}

/// A failed GetExitCodeProcess, or none given, cannot say the process exited: it reads as running.
TEST(WindowsProcessIdentity, UnknownExitCodeReadsAsRunning)
{
    fake() = FakeKernel{};
    fake().exitCode = 1;
    fake().exitCodeError = ERROR_INVALID_HANDLE;
    {
        const auto failed = openProcessVerified(FAKES, {.pid = PID, .startTimeTicks = START_TICKS}, 0);
        ASSERT_TRUE(failed.has_value());
        EXPECT_EQ(fake().exitCodeCalls, 1);
    }
    EXPECT_EQ(fake().closes, 1);

    fake() = FakeKernel{};
    fake().exitCode = 1;
    ProcessIdentityFunctions noExitCheck = FAKES;
    noExitCheck.getExitCodeProcess = nullptr;
    {
        const auto skipped = openProcessVerified(noExitCheck, {.pid = PID, .startTimeTicks = START_TICKS}, 0);
        ASSERT_TRUE(skipped.has_value());
        EXPECT_EQ(fake().exitCodeCalls, 0);
    }
    EXPECT_EQ(fake().closes, 1);
}

/// How a reader reports each refusal, with ModulesReadStatus standing for the readers' status enums.
TEST(WindowsProcessIdentity, ReaderStatusForEachRefusal)
{
    const auto statusOf = [](IdentityFailureKind kind, DWORD error = 0)
    {
        return toReadFailure<ModulesReadStatus>({.kind = kind, .win32Error = error});
    };

    EXPECT_EQ(statusOf(IdentityFailureKind::InvalidPid).status, ModulesReadStatus::ProcessExited);
    EXPECT_EQ(statusOf(IdentityFailureKind::ProcessGone, ERROR_INVALID_PARAMETER).status, ModulesReadStatus::ProcessExited);
    EXPECT_EQ(statusOf(IdentityFailureKind::Mismatch).status, ModulesReadStatus::ProcessExited);
    EXPECT_EQ(statusOf(IdentityFailureKind::Exited).status, ModulesReadStatus::ProcessExited);
    EXPECT_EQ(statusOf(IdentityFailureKind::IdentityUnknown).status, ModulesReadStatus::IdentityUnknown);
    EXPECT_EQ(statusOf(IdentityFailureKind::AccessDenied, ERROR_ACCESS_DENIED).status, ModulesReadStatus::PermissionDenied);
    for (const auto kind : {IdentityFailureKind::InvalidPid,
                            IdentityFailureKind::ProcessGone,
                            IdentityFailureKind::Mismatch,
                            IdentityFailureKind::Exited,
                            IdentityFailureKind::IdentityUnknown,
                            IdentityFailureKind::AccessDenied})
    {
        EXPECT_TRUE(statusOf(kind, ERROR_ACCESS_DENIED).detail.empty());
    }

    // Any other open error, and a failed GetProcessTimes, are Failed with the system's message.
    for (const auto kind : {IdentityFailureKind::OpenFailed, IdentityFailureKind::TimesFailed})
    {
        const auto refused = statusOf(kind, ERROR_NOT_ENOUGH_MEMORY);
        EXPECT_EQ(refused.status, ModulesReadStatus::Failed);
        EXPECT_EQ(refused.detail, std::system_category().message(static_cast<int>(ERROR_NOT_ENOUGH_MEMORY)));
    }
}

/// Start time of `process`, as the probe reports it (GetProcessTimes' creation FILETIME).
std::optional<std::uint64_t> startTicksOf(HANDLE process)
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime) == 0)
    {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

TEST(WindowsProcessIdentity, RealCallsConfirmThisProcessAndRefuseAnotherStartTime)
{
    const std::optional<std::uint64_t> start = startTicksOf(GetCurrentProcess());
    ASSERT_TRUE(start.has_value());
    const auto pid = static_cast<std::int32_t>(GetCurrentProcessId());

    const auto self = openProcessVerified({}, {.pid = pid, .startTimeTicks = start.value_or(0)}, 0);
    ASSERT_TRUE(self.has_value()) << "error " << self.error().win32Error;
    EXPECT_EQ(GetProcessId(self->get()), GetCurrentProcessId());

    const auto other = openProcessVerified({}, {.pid = pid, .startTimeTicks = start.value_or(0) + 1}, 0);
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().kind, IdentityFailureKind::Mismatch);
}

/// A child this test started, waited for and still holds open: exited, but its PID and start time
/// still match (#2206).
TEST(WindowsProcessIdentity, RealCallsRefuseAnExitedChildStillHeldOpen)
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    ASSERT_GT(length, 0U);
    const std::wstring application = std::wstring(system.data(), length) + L"\\cmd.exe";
    std::wstring commandLine = L"cmd.exe /c exit 3";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    ASSERT_NE(CreateProcessW(
                  application.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info),
              0)
        << "error " << GetLastError();
    const UniqueHandle child(info.hProcess); // held open for the whole test, so the PID stays pinned
    const UniqueHandle thread(info.hThread);
    ASSERT_EQ(WaitForSingleObject(child.get(), 30'000), WAIT_OBJECT_0);

    const std::optional<std::uint64_t> start = startTicksOf(child.get());
    ASSERT_TRUE(start.has_value());
    const auto exited =
        openProcessVerified({}, {.pid = static_cast<std::int32_t>(info.dwProcessId), .startTimeTicks = start.value_or(0)}, 0);
    ASSERT_FALSE(exited.has_value());
    EXPECT_EQ(exited.error().kind, IdentityFailureKind::Exited);
}

} // namespace
} // namespace Platform::Windows
