/// @file test_WindowsElevation.cpp
/// @brief The current-token reads (#2275, #3864): readProcessElevated() and readTokenElevation() with both
/// answers and every failure through faked token calls, currentElevationType(), currentUserName() with a
/// name longer than any fixed buffer through a faked LookupAccountSidW, and the real calls' answers for
/// this test process.
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

#include <cstddef>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

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
    static inline TOKEN_ELEVATION_TYPE elevationType = TokenElevationTypeDefault;

    static inline DWORD requestedAccess = 0;
    static inline TOKEN_INFORMATION_CLASS requestedClass = TokenUser;
    static inline int readCalls = 0;

    static void reset()
    {
        openFails = false;
        readFails = false;
        elevated = false;
        elevationType = TokenElevationTypeDefault;
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

    static BOOL WINAPI getTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD length, PDWORD returned)
    {
        ++readCalls;
        requestedClass = cls;
        if (!readFails && cls == TokenUser)
        {
            return GetTokenInformation(token, cls, info, length, returned); // the real user: a SID LookupAccountSidW could name
        }
        if (!readFails && cls == TokenElevationType && length >= sizeof(TOKEN_ELEVATION_TYPE))
        {
            *static_cast<TOKEN_ELEVATION_TYPE*>(info) = elevationType;
            *returned = sizeof(TOKEN_ELEVATION_TYPE);
            return TRUE;
        }
        if (readFails || cls != TokenElevation || length < sizeof(TOKEN_ELEVATION))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        static_cast<TOKEN_ELEVATION*>(info)->TokenIsElevated = elevated ? 1 : 0;
        *returned = sizeof(TOKEN_ELEVATION);
        return TRUE;
    }

    [[nodiscard]] static CurrentTokenFunctions functions()
    {
        return {.openProcessToken = &openProcessToken, .getTokenInformation = &getTokenInformation, .lookupAccountSid = &lookupAccountSid};
    }

    // The account the faked LookupAccountSidW names, and how it behaves.
    static inline std::wstring accountUser;
    static inline std::wstring accountDomain;
    static inline std::wstring renamedUser;          ///< Non-empty: the name grows to this after the sizing call
    static inline DWORD lookupError = ERROR_SUCCESS; ///< Not ERROR_SUCCESS: every lookup fails with it
    static inline int lookupCalls = 0;

    static void resetAccount()
    {
        accountUser.clear();
        accountDomain = L"PC";
        renamedUser.clear();
        lookupError = ERROR_SUCCESS;
        lookupCalls = 0;
    }

    /// As LookupAccountSidW: buffers too short (or null) give ERROR_INSUFFICIENT_BUFFER and the lengths
    /// needed, each with its NUL; buffers long enough get the names and their lengths without the NUL.
    static BOOL WINAPI lookupAccountSid(
        LPCWSTR /*system*/, PSID /*sid*/, LPWSTR name, LPDWORD nameChars, LPWSTR domain, LPDWORD domainChars, PSID_NAME_USE use)
    {
        ++lookupCalls;
        if (lookupError != ERROR_SUCCESS)
        {
            SetLastError(lookupError);
            return FALSE;
        }
        if (lookupCalls > 1 && !renamedUser.empty())
        {
            accountUser = renamedUser; // renamed between the sizing call and the read
            renamedUser.clear();
        }
        const auto nameNeeded = static_cast<DWORD>(accountUser.size() + 1);
        const auto domainNeeded = static_cast<DWORD>(accountDomain.size() + 1);
        if (name == nullptr || domain == nullptr || *nameChars < nameNeeded || *domainChars < domainNeeded)
        {
            *nameChars = nameNeeded;
            *domainChars = domainNeeded;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        std::wmemcpy(name, accountUser.c_str(), nameNeeded);
        std::wmemcpy(domain, accountDomain.c_str(), domainNeeded);
        *nameChars = nameNeeded - 1;
        *domainChars = domainNeeded - 1;
        *use = SidTypeUser;
        return TRUE;
    }
};

class WindowsElevationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeToken::reset();
        FakeToken::resetAccount();
    }
};

TEST_F(WindowsElevationTest, ElevatedTokenReportsElevated)
{
    FakeToken::elevated = true;

    EXPECT_TRUE(readProcessElevated(FakeToken::functions()));
    EXPECT_EQ(FakeToken::requestedAccess, static_cast<DWORD>(TOKEN_QUERY));
    EXPECT_EQ(FakeToken::requestedClass, TokenElevation);
    EXPECT_EQ(FakeToken::readCalls, 1);
}

TEST_F(WindowsElevationTest, UnelevatedTokenReportsNotElevated)
{
    FakeToken::elevated = false;

    EXPECT_FALSE(readProcessElevated(FakeToken::functions()));
    EXPECT_EQ(FakeToken::readCalls, 1);
}

TEST_F(WindowsElevationTest, UnopenableTokenReportsNotElevatedWithoutReading)
{
    FakeToken::openFails = true;
    FakeToken::elevated = true;

    EXPECT_FALSE(readProcessElevated(FakeToken::functions()));
    EXPECT_EQ(FakeToken::readCalls, 0);
}

TEST_F(WindowsElevationTest, UnreadableElevationReportsNotElevated)
{
    FakeToken::readFails = true;
    FakeToken::elevated = true;

    EXPECT_FALSE(readProcessElevated(FakeToken::functions()));
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

TEST_F(WindowsElevationTest, CurrentElevationTypeReadsTheTokenAndDefaultsOnFailure)
{
    FakeToken::elevationType = TokenElevationTypeLimited;
    EXPECT_EQ(currentElevationType(FakeToken::functions()), TokenElevationTypeLimited);
    EXPECT_EQ(FakeToken::requestedClass, TokenElevationType);

    FakeToken::elevationType = TokenElevationTypeFull;
    EXPECT_EQ(currentElevationType(FakeToken::functions()), TokenElevationTypeFull);

    FakeToken::readFails = true;
    EXPECT_EQ(currentElevationType(FakeToken::functions()), TokenElevationTypeDefault);

    FakeToken::readFails = false;
    FakeToken::openFails = true;
    EXPECT_EQ(currentElevationType(FakeToken::functions()), TokenElevationTypeDefault);
}

// #3864: WindowsScheduledTasks named the user with fixed 256-wchar buffers, so a longer name failed the
// lookup. currentUserName() sizes its buffers from the call itself.
TEST_F(WindowsElevationTest, CurrentUserNameReadsANameLongerThan256Characters)
{
    FakeToken::accountUser = std::wstring(300, L'n');
    FakeToken::accountDomain = std::wstring(270, L'd');

    const std::optional<AccountName> account = currentUserName(FakeToken::functions());

    ASSERT_TRUE(account.has_value());
    const AccountName named = account.value_or(AccountName{});
    EXPECT_EQ(named.user, std::wstring(300, L'n'));
    EXPECT_EQ(named.domain, std::wstring(270, L'd'));
    EXPECT_EQ(FakeToken::lookupCalls, 2) << "one call to size the buffers, one to read the names";
}

TEST_F(WindowsElevationTest, CurrentUserNameSizesAgainForANameThatGrewBetweenTheCalls)
{
    FakeToken::accountUser = L"Matt";
    FakeToken::renamedUser = std::wstring(400, L'r');

    const std::optional<AccountName> account = currentUserName(FakeToken::functions());

    ASSERT_TRUE(account.has_value());
    const AccountName named = account.value_or(AccountName{});
    EXPECT_EQ(named.user, std::wstring(400, L'r'));
    EXPECT_EQ(named.domain, L"PC");
    EXPECT_EQ(FakeToken::lookupCalls, 3);
}

TEST_F(WindowsElevationTest, CurrentUserNameIsEmptyWhenTheLookupOrTheTokenFails)
{
    FakeToken::accountUser = L"Matt";
    FakeToken::lookupError = ERROR_NONE_MAPPED;
    EXPECT_EQ(currentUserName(FakeToken::functions()), std::nullopt);

    FakeToken::lookupError = ERROR_SUCCESS;
    FakeToken::lookupCalls = 0;
    FakeToken::readFails = true;
    EXPECT_EQ(currentUserName(FakeToken::functions()), std::nullopt);
    EXPECT_EQ(FakeToken::lookupCalls, 0) << "no SID, nothing to look up";

    FakeToken::readFails = false;
    FakeToken::openFails = true;
    EXPECT_EQ(currentUserName(FakeToken::functions()), std::nullopt);
    EXPECT_EQ(FakeToken::lookupCalls, 0);
}

TEST_F(WindowsElevationTest, CurrentUserSidReportsTheErrorWhenTheTokenCantBeOpened)
{
    FakeToken::openFails = true;
    const auto sid = currentUserSid(FakeToken::functions());
    ASSERT_FALSE(sid.has_value());
    EXPECT_EQ(sid.error(), static_cast<DWORD>(ERROR_ACCESS_DENIED));
    const auto text = currentUserSidText(FakeToken::functions());
    ASSERT_FALSE(text.has_value());
    EXPECT_EQ(text.error(), static_cast<DWORD>(ERROR_ACCESS_DENIED));
}

TEST(WindowsElevationRealTest, CurrentUserSidIsThisProcessTokensUser)
{
    HANDLE token = nullptr;
    ASSERT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), FALSE);
    DWORD size = 0;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0, &size));
    std::vector<std::byte> buffer(size);
    const BOOL read = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    ASSERT_NE(read, FALSE);
    const auto* const user = reinterpret_cast<const TOKEN_USER*>(
        buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - TokenUser's buffer is a TOKEN_USER

    auto sid = currentUserSid();
    ASSERT_TRUE(sid.has_value());
    EXPECT_NE(EqualSid(static_cast<PSID>(sid->data()), user->User.Sid), FALSE);
    const auto text = currentUserSidText();
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(text->starts_with(L"S-1-")) << "a SID's text form";
    const std::optional<AccountName> name = currentUserName();
    ASSERT_TRUE(name.has_value());
    EXPECT_FALSE(name.value_or(AccountName{}).user.empty());
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
