/// @file test_WindowsElevation.cpp
/// @brief isProcessElevated() and readTokenElevation() (#2275): both answers and every failure through
/// faked token calls, and the real calls' answer for this test process.
///
/// The fakes hand out this test process's real token opened for query only, so UniqueHandle closes a
/// real handle, while the elevation answer comes from the fake.

#include "Platform/Windows/WindowsElevation.h"

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

#include <optional>

namespace Platform::Windows
{
namespace
{

/// What the faked calls do and saw. Function pointers can't capture, so the fakes share this state;
/// each test starts from reset().
struct FakeToken
{
    static inline bool openFails = false;
    static inline bool readFails = false;
    static inline bool elevated = false;

    static inline DWORD requestedAccess = 0;
    static inline TOKEN_INFORMATION_CLASS requestedClass = TokenUser;
    static inline int readCalls = 0;

    static void reset()
    {
        openFails = false;
        readFails = false;
        elevated = false;
        requestedAccess = 0;
        requestedClass = TokenUser;
        readCalls = 0;
    }

    static BOOL WINAPI openProcessToken(HANDLE /*process*/, DWORD access, PHANDLE token)
    {
        requestedAccess = access;
        if (openFails)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token);
    }

    static BOOL WINAPI getTokenInformation(HANDLE /*token*/, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD length, PDWORD returned)
    {
        ++readCalls;
        requestedClass = cls;
        if (readFails || cls != TokenElevation || length < sizeof(TOKEN_ELEVATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        static_cast<TOKEN_ELEVATION*>(info)->TokenIsElevated = elevated ? 1 : 0;
        *returned = sizeof(TOKEN_ELEVATION);
        return TRUE;
    }

    [[nodiscard]] static ElevationFunctions functions()
    {
        return {.openProcessToken = &openProcessToken, .getTokenInformation = &getTokenInformation};
    }
};

class WindowsElevationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeToken::reset();
    }
};

TEST_F(WindowsElevationTest, ElevatedTokenReportsElevated)
{
    FakeToken::elevated = true;

    EXPECT_TRUE(isProcessElevated(FakeToken::functions()));
    EXPECT_EQ(FakeToken::requestedAccess, static_cast<DWORD>(TOKEN_QUERY));
    EXPECT_EQ(FakeToken::requestedClass, TokenElevation);
    EXPECT_EQ(FakeToken::readCalls, 1);
}

TEST_F(WindowsElevationTest, UnelevatedTokenReportsNotElevated)
{
    FakeToken::elevated = false;

    EXPECT_FALSE(isProcessElevated(FakeToken::functions()));
    EXPECT_EQ(FakeToken::readCalls, 1);
}

TEST_F(WindowsElevationTest, UnopenableTokenReportsNotElevatedWithoutReading)
{
    FakeToken::openFails = true;
    FakeToken::elevated = true;

    EXPECT_FALSE(isProcessElevated(FakeToken::functions()));
    EXPECT_EQ(FakeToken::readCalls, 0);
}

TEST_F(WindowsElevationTest, UnreadableElevationReportsNotElevated)
{
    FakeToken::readFails = true;
    FakeToken::elevated = true;

    EXPECT_FALSE(isProcessElevated(FakeToken::functions()));
}

TEST_F(WindowsElevationTest, ReadTokenElevationGivesBothAnswersAndEmptyOnFailure)
{
    HANDLE token = nullptr;
    ASSERT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), FALSE);

    FakeToken::elevated = true;
    EXPECT_EQ(readTokenElevation(token, &FakeToken::getTokenInformation), std::optional<bool>{true});

    FakeToken::elevated = false;
    EXPECT_EQ(readTokenElevation(token, &FakeToken::getTokenInformation), std::optional<bool>{false});

    // A failed read leaves GetLastError() as the call set it, for the caller to report.
    FakeToken::readFails = true;
    EXPECT_EQ(readTokenElevation(token, &FakeToken::getTokenInformation), std::nullopt);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_INVALID_PARAMETER));

    CloseHandle(token);
}

TEST(WindowsElevationRealTest, RealCallMatchesThisProcessToken)
{
    HANDLE token = nullptr;
    ASSERT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), FALSE);
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL read = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    ASSERT_NE(read, FALSE);
    const bool expected = elevation.TokenIsElevated != 0;

    EXPECT_EQ(isProcessElevated(), expected);
    EXPECT_EQ(isProcessElevated(), expected) << "the answer is constant for the process lifetime";
}

} // namespace
} // namespace Platform::Windows
