/// @file test_WindowsDebugPrivilege.cpp
/// @brief enableDebugPrivilegeIfElevated() through faked token calls (#2128): SeDebugPrivilege is
/// enabled when, and only when, the token is elevated, and every failure is reported, not thrown.
///
/// The fakes never adjust a real token: OpenProcessToken hands out this test process's token opened
/// for query only, so it can be closed like the real one, and AdjustTokenPrivileges is always faked.

#include "Platform/Windows/WindowsDebugPrivilege.h"

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

#include <string>

namespace Platform::Windows
{
namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FakeToken
{
    static inline bool openFails = false;
    static inline bool elevationFails = false;
    static inline bool elevated = true;
    static inline bool lookupFails = false;
    static inline bool adjustFails = false;
    static inline DWORD adjustLastError = ERROR_SUCCESS;

    static inline DWORD requestedAccess = 0;
    static inline std::wstring lookedUpName;
    static inline int adjustCalls = 0;
    static inline DWORD adjustedCount = 0;
    static inline DWORD adjustedAttributes = 0;
    static inline LUID adjustedLuid{};

    static constexpr LUID DEBUG_LUID{.LowPart = 20, .HighPart = 0};

    static void reset()
    {
        openFails = false;
        elevationFails = false;
        elevated = true;
        lookupFails = false;
        adjustFails = false;
        adjustLastError = ERROR_SUCCESS;
        requestedAccess = 0;
        lookedUpName.clear();
        adjustCalls = 0;
        adjustedCount = 0;
        adjustedAttributes = 0;
        adjustedLuid = {};
    }

    static BOOL WINAPI openProcessToken(HANDLE /*process*/, DWORD access, PHANDLE token)
    {
        requestedAccess = access;
        if (openFails)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        // A real handle for UniqueHandle to close, opened for query only: nothing can adjust it.
        return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token);
    }

    static BOOL WINAPI getTokenInformation(HANDLE /*token*/, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD length, PDWORD returned)
    {
        if (elevationFails || cls != TokenElevation || length < sizeof(TOKEN_ELEVATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        static_cast<TOKEN_ELEVATION*>(info)->TokenIsElevated = elevated ? 1 : 0;
        *returned = sizeof(TOKEN_ELEVATION);
        return TRUE;
    }

    static BOOL WINAPI lookupPrivilegeValue(LPCWSTR /*system*/, LPCWSTR name, PLUID luid)
    {
        lookedUpName = name;
        if (lookupFails)
        {
            SetLastError(ERROR_NO_SUCH_PRIVILEGE);
            return FALSE;
        }
        *luid = DEBUG_LUID;
        return TRUE;
    }

    static BOOL WINAPI adjustTokenPrivileges(HANDLE /*token*/,
                                             BOOL disableAll,
                                             PTOKEN_PRIVILEGES newState,
                                             DWORD /*length*/,
                                             PTOKEN_PRIVILEGES /*previous*/,
                                             PDWORD /*returned*/)
    {
        ++adjustCalls;
        EXPECT_EQ(disableAll, FALSE);
        adjustedCount = newState->PrivilegeCount;
        adjustedLuid = newState->Privileges[0].Luid;
        adjustedAttributes = newState->Privileges[0].Attributes;
        if (adjustFails)
        {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        SetLastError(adjustLastError);
        return TRUE;
    }

    [[nodiscard]] static DebugPrivilegeFunctions functions()
    {
        return {.openProcessToken = &openProcessToken,
                .getTokenInformation = &getTokenInformation,
                .lookupPrivilegeValue = &lookupPrivilegeValue,
                .adjustTokenPrivileges = &adjustTokenPrivileges};
    }
};

class WindowsDebugPrivilegeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeToken::reset();
    }
};

TEST_F(WindowsDebugPrivilegeTest, ElevatedTokenEnablesSeDebugPrivilege)
{
    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::Enabled);

    EXPECT_EQ(FakeToken::requestedAccess, static_cast<DWORD>(TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY));
    EXPECT_EQ(FakeToken::lookedUpName, std::wstring(SE_DEBUG_NAME));
    ASSERT_EQ(FakeToken::adjustCalls, 1);
    EXPECT_EQ(FakeToken::adjustedCount, 1U);
    EXPECT_EQ(FakeToken::adjustedLuid.LowPart, FakeToken::DEBUG_LUID.LowPart);
    EXPECT_EQ(FakeToken::adjustedLuid.HighPart, FakeToken::DEBUG_LUID.HighPart);
    EXPECT_EQ(FakeToken::adjustedAttributes, static_cast<DWORD>(SE_PRIVILEGE_ENABLED));
}

TEST_F(WindowsDebugPrivilegeTest, UnelevatedTokenIsLeftUnchanged)
{
    FakeToken::elevated = false;

    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::NotElevated);
    EXPECT_TRUE(FakeToken::lookedUpName.empty());
    EXPECT_EQ(FakeToken::adjustCalls, 0);
}

TEST_F(WindowsDebugPrivilegeTest, NotAllAssignedIsReportedAlthoughAdjustSucceeded)
{
    FakeToken::adjustLastError = ERROR_NOT_ALL_ASSIGNED;

    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::NotAssigned);
    EXPECT_EQ(FakeToken::adjustCalls, 1);
}

TEST_F(WindowsDebugPrivilegeTest, AdjustFailureIsReported)
{
    FakeToken::adjustFails = true;

    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::AdjustFailed);
}

TEST_F(WindowsDebugPrivilegeTest, LookupFailureSkipsAdjust)
{
    FakeToken::lookupFails = true;

    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::LookupFailed);
    EXPECT_EQ(FakeToken::adjustCalls, 0);
}

TEST_F(WindowsDebugPrivilegeTest, UnopenableTokenChangesNothing)
{
    FakeToken::openFails = true;

    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::TokenUnavailable);
    EXPECT_TRUE(FakeToken::lookedUpName.empty());
    EXPECT_EQ(FakeToken::adjustCalls, 0);
}

TEST_F(WindowsDebugPrivilegeTest, UnreadableElevationChangesNothing)
{
    FakeToken::elevationFails = true;

    EXPECT_EQ(enableDebugPrivilegeIfElevated(FakeToken::functions()), DebugPrivilegeResult::TokenUnavailable);
    EXPECT_EQ(FakeToken::adjustCalls, 0);
}

} // namespace
} // namespace Platform::Windows
