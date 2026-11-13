/// @file test_WindowsSessionAccount.cpp
/// @brief Whether an elevated TaskSmack runs as another account than the session's signed-in user (#3671):
/// readSessionAccount() through faked calls, for the same account (in any case), another account, and a user
/// that can't be read (either one, or an empty session user), which count as the same account; and the real
/// calls' answer for this test process, which runs as the signed-in user.

#include "Platform/SessionAccount.h"
#include "Platform/Windows/WindowsSessionAccount.h"

#include <gtest/gtest.h>

#include <optional>

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
    static inline int processReads = 0;
    static inline int sessionReads = 0;

    static void reset()
    {
        elevated = true;
        process = AccountName{.domain = L"PC", .user = L"Admin"};
        session = AccountName{.domain = L"PC", .user = L"Matt"};
        processReads = 0;
        sessionReads = 0;
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
};

constexpr SessionAccountFunctions FAKE_FNS{
    .isElevated = &FakeAccounts::isElevated,
    .processUser = &FakeAccounts::processUser,
    .sessionUser = &FakeAccounts::sessionUser,
};

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

} // namespace
} // namespace Platform::Windows
