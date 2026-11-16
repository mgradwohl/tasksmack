/// @file test_WindowsSessionAccount.cpp
/// @brief Whether an elevated TaskSmack runs as another account than the session's signed-in user (#3671):
/// readSessionAccount() through faked calls, for the same account (in any case), another account, and a user
/// that can't be read (either one, or an empty session user), which count as the same account; by SID when
/// both SIDs can be read and by name when either can't (#4077); lookupUserSid() through a faked
/// LookupAccountNameW; and the real calls' answer for this test process, which runs as the signed-in user.

#include "Platform/SessionAccount.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsSessionAccount.h"

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

namespace Platform::Windows
{
namespace
{

/// What the faked calls answer and how often they were asked. Function pointers can't capture, so the fakes
/// share this state; each test starts from reset().
struct FakeAccounts
{
    static inline bool elevated = true;
    static inline std::optional<AccountName> process;
    static inline std::optional<AccountName> session;
    // No SIDs unless a test gives them: the name comparison, as before #4077.
    static inline std::optional<SidBytes> processSid;
    static inline std::optional<SidBytes> sessionSid;
    static inline int processReads = 0;
    static inline int sessionReads = 0;
    static inline int processSidReads = 0;
    static inline int sessionSidReads = 0;
    static inline std::optional<AccountName> lookedUp;

    static void reset()
    {
        elevated = true;
        process = AccountName{.domain = L"PC", .user = L"Admin"};
        session = AccountName{.domain = L"PC", .user = L"Matt"};
        processSid.reset();
        sessionSid.reset();
        processReads = 0;
        sessionReads = 0;
        processSidReads = 0;
        sessionSidReads = 0;
        lookedUp.reset();
    }

    static bool isElevated()
    {
        return elevated;
    }
    static std::optional<AccountName> processUser()
    {
        ++processReads;
        return process;
    }
    static std::optional<AccountName> sessionUser()
    {
        ++sessionReads;
        return session;
    }
    static std::optional<SidBytes> processUserSid()
    {
        ++processSidReads;
        return processSid;
    }
    static std::optional<SidBytes> accountSid(const AccountName& account)
    {
        ++sessionSidReads;
        lookedUp = account;
        return sessionSid;
    }
};

constexpr SessionAccountFunctions FAKE_FNS{
    .isElevated = &FakeAccounts::isElevated,
    .processUser = &FakeAccounts::processUser,
    .sessionUser = &FakeAccounts::sessionUser,
    .processUserSid = &FakeAccounts::processUserSid,
    .accountSid = &FakeAccounts::accountSid,
};

/// Two SIDs that differ; their bytes are all readSessionAccount() compares.
[[nodiscard]] SidBytes sidA()
{
    return {std::byte{1}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{5}};
}
[[nodiscard]] SidBytes sidB()
{
    return {std::byte{1}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{6}};
}

class WindowsSessionAccountTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeAccounts::reset();
    }
    void TearDown() override
    {
        FakeAccounts::reset();
    }
};

TEST_F(WindowsSessionAccountTest, AnotherAccountIsNamed)
{
    const SessionAccount account = readSessionAccount(FAKE_FNS);
    EXPECT_TRUE(account.otherAccount);
    EXPECT_EQ(account.account, "Admin");
}

TEST_F(WindowsSessionAccountTest, TheSameAccountIsTheUsers)
{
    FakeAccounts::process = AccountName{.domain = L"PC", .user = L"Matt"};
    EXPECT_EQ(readSessionAccount(FAKE_FNS), SessionAccount{});
}

TEST_F(WindowsSessionAccountTest, NamesCompareIgnoringCaseAsWindowsDoes)
{
    FakeAccounts::process = AccountName{.domain = L"pc", .user = L"MATT"};
    EXPECT_FALSE(readSessionAccount(FAKE_FNS).otherAccount);
    EXPECT_TRUE(sameAccount({.domain = L"AzureAD", .user = L"Jürgen"}, {.domain = L"AZUREAD", .user = L"JÜRGEN"}));
}

TEST_F(WindowsSessionAccountTest, TheSameNameInAnotherDomainIsAnotherAccount)
{
    FakeAccounts::process = AccountName{.domain = L"CONTOSO", .user = L"Matt"};
    const SessionAccount account = readSessionAccount(FAKE_FNS);
    EXPECT_TRUE(account.otherAccount);
    EXPECT_EQ(account.account, "Matt");
}

TEST_F(WindowsSessionAccountTest, AnUnreadableSessionUserCountsAsTheSameAccount)
{
    FakeAccounts::session.reset();
    EXPECT_EQ(readSessionAccount(FAKE_FNS), SessionAccount{});
    EXPECT_EQ(FakeAccounts::sessionReads, 1);
}

TEST_F(WindowsSessionAccountTest, ASessionNoOneSignedInToCountsAsTheSameAccount)
{
    FakeAccounts::session = AccountName{}; // WTS gives an empty user for session 0
    EXPECT_FALSE(readSessionAccount(FAKE_FNS).otherAccount);
}

TEST_F(WindowsSessionAccountTest, AnUnreadableTokenUserCountsAsTheSameAccount)
{
    FakeAccounts::process.reset();
    EXPECT_EQ(readSessionAccount(FAKE_FNS), SessionAccount{});
    EXPECT_EQ(FakeAccounts::sessionReads, 0) << "nothing to compare it with";
}

TEST_F(WindowsSessionAccountTest, AStandardTaskSmackIsAlwaysTheUsersAndReadsNoAccount)
{
    FakeAccounts::elevated = false;
    EXPECT_EQ(readSessionAccount(FAKE_FNS), SessionAccount{});
    EXPECT_EQ(FakeAccounts::processReads, 0);
    EXPECT_EQ(FakeAccounts::sessionReads, 0);
    EXPECT_EQ(FakeAccounts::processSidReads, 0);
    EXPECT_EQ(FakeAccounts::sessionSidReads, 0);
}

TEST_F(WindowsSessionAccountTest, TheSameSidUnderAnotherNameIsTheSameAccount)
{
    // A rename, or a WTS name that differs from what the SID looks up as: the SID decides, as the helper's does.
    FakeAccounts::process = AccountName{.domain = L"PC", .user = L"Matthew"};
    FakeAccounts::processSid = sidA();
    FakeAccounts::sessionSid = sidA();
    EXPECT_EQ(readSessionAccount(FAKE_FNS), SessionAccount{});
}

TEST_F(WindowsSessionAccountTest, AnotherSidUnderTheSameNameIsAnotherAccount)
{
    FakeAccounts::process = AccountName{.domain = L"PC", .user = L"Matt"};
    FakeAccounts::processSid = sidA();
    FakeAccounts::sessionSid = sidB();
    const SessionAccount account = readSessionAccount(FAKE_FNS);
    EXPECT_TRUE(account.otherAccount);
    EXPECT_EQ(account.account, "Matt");
}

TEST_F(WindowsSessionAccountTest, SidsThatAgreeWithTheNamesGiveTheSameAnswers)
{
    FakeAccounts::processSid = sidA();
    FakeAccounts::sessionSid = sidB();
    const SessionAccount other = readSessionAccount(FAKE_FNS);
    EXPECT_TRUE(other.otherAccount);
    EXPECT_EQ(other.account, "Admin");

    FakeAccounts::process = AccountName{.domain = L"PC", .user = L"Matt"};
    FakeAccounts::sessionSid = sidA();
    EXPECT_EQ(readSessionAccount(FAKE_FNS), SessionAccount{});
}

TEST_F(WindowsSessionAccountTest, TheSessionsSidIsLookedUpByItsWtsName)
{
    FakeAccounts::processSid = sidA();
    FakeAccounts::sessionSid = sidA();
    static_cast<void>(readSessionAccount(FAKE_FNS));
    ASSERT_TRUE(FakeAccounts::lookedUp.has_value());
    EXPECT_EQ(FakeAccounts::lookedUp.value_or(AccountName{}).domain, L"PC");
    EXPECT_EQ(FakeAccounts::lookedUp.value_or(AccountName{}).user, L"Matt");
}

TEST_F(WindowsSessionAccountTest, AnUnresolvableSessionSidFallsBackToNames)
{
    FakeAccounts::processSid = sidA(); // the session's lookup fails: sessionSid stays empty
    EXPECT_TRUE(readSessionAccount(FAKE_FNS).otherAccount);
    EXPECT_EQ(FakeAccounts::sessionSidReads, 1);

    FakeAccounts::process = AccountName{.domain = L"pc", .user = L"MATT"};
    EXPECT_FALSE(readSessionAccount(FAKE_FNS).otherAccount);
}

TEST_F(WindowsSessionAccountTest, AnUnreadableProcessSidFallsBackToNamesWithoutALookup)
{
    FakeAccounts::sessionSid = sidA();
    EXPECT_TRUE(readSessionAccount(FAKE_FNS).otherAccount);
    EXPECT_EQ(FakeAccounts::sessionSidReads, 0) << "nothing to compare it with";

    FakeAccounts::process = AccountName{.domain = L"PC", .user = L"Matt"};
    EXPECT_FALSE(readSessionAccount(FAKE_FNS).otherAccount);
}

/// A LookupAccountNameW that answers as Windows does: a first call too small for the SID asks for its size
/// (ERROR_INSUFFICIENT_BUFFER); then LocalSystem's SID, the domain, and the type.
struct FakeLookup
{
    static inline std::wstring asked;
    static inline SID_NAME_USE type = SidTypeUser;
    static inline bool fails = false;
    static inline int calls = 0;
    static inline const wchar_t* domain = L"PC";

    static void reset()
    {
        asked.clear();
        type = SidTypeUser;
        fails = false;
        calls = 0;
    }

    static BOOL WINAPI
    lookup(LPCWSTR /*system*/, LPCWSTR name, PSID sid, LPDWORD sidSize, LPWSTR domainOut, LPDWORD domainLength, PSID_NAME_USE use)
    {
        ++calls;
        asked = name;
        if (fails)
        {
            SetLastError(ERROR_NONE_MAPPED);
            return FALSE;
        }
        const DWORD needed = GetSidLengthRequired(1);
        const auto domainNeeded = static_cast<DWORD>(std::wcslen(domain) + 1);
        if (sid == nullptr || *sidSize < needed || *domainLength < domainNeeded)
        {
            *sidSize = needed;
            *domainLength = domainNeeded;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        DWORD size = *sidSize;
        if (CreateWellKnownSid(WinLocalSystemSid, nullptr, sid, &size) == FALSE)
        {
            return FALSE;
        }
        std::wmemcpy(domainOut, domain, domainNeeded);
        *domainLength = domainNeeded - 1;
        *use = type;
        return TRUE;
    }
};

class LookupUserSidTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        FakeLookup::reset();
    }
    void TearDown() override
    {
        FakeLookup::reset();
    }
};

TEST_F(LookupUserSidTest, AQualifiedNameResolvesToItsSid)
{
    const std::optional<SidBytes> sid = lookupUserSid({.domain = L"PC", .user = L"Matt"}, &FakeLookup::lookup);
    ASSERT_TRUE(sid.has_value());
    EXPECT_EQ(FakeLookup::asked, L"PC\\Matt");
    EXPECT_EQ(FakeLookup::calls, 2) << "one call for the sizes, one for the SID";

    SidBytes system(SECURITY_MAX_SID_SIZE);
    auto size = static_cast<DWORD>(system.size());
    ASSERT_NE(CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &size), FALSE);
    system.resize(size);
    EXPECT_EQ(sid.value_or(SidBytes{}), system);
}

TEST_F(LookupUserSidTest, AUserWithNoDomainIsLookedUpAlone)
{
    EXPECT_TRUE(lookupUserSid({.domain = L"", .user = L"Matt"}, &FakeLookup::lookup).has_value());
    EXPECT_EQ(FakeLookup::asked, L"Matt");
}

TEST_F(LookupUserSidTest, ANameThatIsntAUsersResolvesToNothing)
{
    FakeLookup::type = SidTypeGroup;
    EXPECT_FALSE(lookupUserSid({.domain = L"PC", .user = L"Matt"}, &FakeLookup::lookup).has_value());
}

TEST_F(LookupUserSidTest, AFailedLookupResolvesToNothing)
{
    FakeLookup::fails = true;
    EXPECT_FALSE(lookupUserSid({.domain = L"PC", .user = L"Matt"}, &FakeLookup::lookup).has_value());
    EXPECT_EQ(FakeLookup::calls, 1);
}

TEST(WindowsSessionAccountRealTest, TheTestProcessRunsAsANamedUser)
{
    const std::optional<AccountName> process = readProcessUser();
    ASSERT_TRUE(process.has_value());
    EXPECT_FALSE(process.value_or(AccountName{}).user.empty());
    // The tests run as the user who started them; either way, whatever the session, the answer is a valid one.
    const SessionAccount account = readSessionAccount();
    EXPECT_EQ(account.otherAccount, !account.account.empty());
}

TEST(WindowsSessionAccountRealTest, TheTestProcessUsersNameResolvesToItsOwnSid)
{
    const std::optional<AccountName> process = readProcessUser();
    const std::optional<SidBytes> sid = readProcessUserSid();
    ASSERT_TRUE(process.has_value());
    ASSERT_TRUE(sid.has_value());
    EXPECT_EQ(readAccountSid(process.value_or(AccountName{})), sid);
}

} // namespace
} // namespace Platform::Windows
