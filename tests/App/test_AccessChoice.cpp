/// @file test_AccessChoice.cpp
/// @brief Standard or Administrator (#2728): the startup notice's decision over every combination of
/// platform, elevation, token, helper and setting, and each platform's wording, checked in one run.

#include "App/AccessChoice.h"
#include "App/AccessTable.h"
#include "App/Panels/AuthorizeWording.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace App::AccessChoice
{
namespace
{

constexpr std::array<AccessPlatform, 2> PLATFORMS{AccessPlatform::Windows, AccessPlatform::Linux};
constexpr std::array<TokenType, 2> TOKENS{TokenType::AdminCapable, TokenType::StandardUser};
constexpr std::array<bool, 2> BOTH{false, true};

/// Calls @p check with every combination of inputs.
template<typename Check> void forEveryInput(const Check& check)
{
    for (const AccessPlatform platform : PLATFORMS)
    {
        for (const bool elevated : BOTH)
        {
            for (const TokenType token : TOKENS)
            {
                for (const bool helperInstalled : BOTH)
                {
                    for (const bool noticeEnabled : BOTH)
                    {
                        check(NoticeInputs{.platform = platform,
                                           .elevated = elevated,
                                           .token = token,
                                           .helperInstalled = helperInstalled,
                                           .noticeEnabled = noticeEnabled});
                    }
                }
            }
        }
    }
}

TEST(AccessChoiceTest, StandardIsTheDefaultInEveryCase)
{
    // Decision 2: TaskSmack always starts as Standard and never chooses Administrator on its own.
    forEveryInput([](const NoticeInputs& inputs) { EXPECT_EQ(decideNotice(inputs).defaultChoice, Choice::Standard); });
}

TEST(AccessChoiceTest, TheNoticeShowsOnlyWhileNotElevatedAndTurnedOn)
{
    forEveryInput([](const NoticeInputs& inputs) { EXPECT_EQ(decideNotice(inputs).show, !inputs.elevated && inputs.noticeEnabled); });
}

TEST(AccessChoiceTest, LinuxOffersAdministratorOnlyWithTheHelperAndSaysHowToGetIt)
{
    const NoticeDecision with = decideNotice({.platform = AccessPlatform::Linux, .helperInstalled = true});
    EXPECT_TRUE(with.administrator.enabled);
    EXPECT_TRUE(with.administrator.note.empty());

    const NoticeDecision without = decideNotice({.platform = AccessPlatform::Linux, .helperInstalled = false});
    EXPECT_FALSE(without.administrator.enabled);
    EXPECT_TRUE(without.administrator.note.contains(".deb or .rpm"));
    EXPECT_TRUE(without.administrator.note.contains("sudo")); // today's remedy stays
}

TEST(AccessChoiceTest, WindowsOffersAdministratorWithoutAHelperAndWarnsAStandardUser)
{
    // Windows' Administrator restarts TaskSmack elevated: the zip, with no helper, offers it too.
    const NoticeDecision admin =
        decideNotice({.platform = AccessPlatform::Windows, .token = TokenType::AdminCapable, .helperInstalled = false});
    EXPECT_TRUE(admin.administrator.enabled);
    EXPECT_TRUE(admin.administrator.note.empty());

    const NoticeDecision standard = decideNotice({.platform = AccessPlatform::Windows, .token = TokenType::StandardUser});
    EXPECT_TRUE(standard.administrator.enabled);
    EXPECT_EQ(standard.administrator.note, STANDARD_USER_NOTE);
}

TEST(AccessChoiceTest, TheTokenMattersOnlyOnWindowsAndTheHelperOnlyOnLinux)
{
    forEveryInput(
        [](const NoticeInputs& inputs)
        {
            NoticeInputs other = inputs;
            if (inputs.platform == AccessPlatform::Linux)
            {
                other.token = inputs.token == TokenType::AdminCapable ? TokenType::StandardUser : TokenType::AdminCapable;
            }
            else
            {
                other.helperInstalled = !inputs.helperInstalled;
            }
            const NoticeDecision a = decideNotice(inputs);
            const NoticeDecision b = decideNotice(other);
            EXPECT_EQ(a.show, b.show);
            EXPECT_EQ(a.administrator.enabled, b.administrator.enabled);
            EXPECT_EQ(a.administrator.note, b.administrator.note);
        });
}

TEST(AccessChoiceTest, EachPlatformSaysItInItsOwnWords)
{
    using AuthorizeWording::wordingFor;
    EXPECT_STREQ(wordingFor(AccessPlatform::Windows).launch, "Run as administrator");
    EXPECT_STREQ(wordingFor(AccessPlatform::Windows).switchSession, "Restart as administrator...");
    EXPECT_TRUE(std::string_view{wordingFor(AccessPlatform::Windows).promptTooltip}.contains("UAC"));
    EXPECT_STREQ(wordingFor(AccessPlatform::Linux).launch, "Run with administrator access");
    EXPECT_STREQ(wordingFor(AccessPlatform::Linux).switchSession, "Authenticate for this session...");
    EXPECT_TRUE(std::string_view{wordingFor(AccessPlatform::Linux).promptTooltip}.contains("polkit"));
}

TEST(AccessChoiceTest, TheHostsButtonWordsAreItsRowOfTheTable)
{
    EXPECT_STREQ(AuthorizeWording::RETRY_TEXT, AuthorizeWording::wordingFor(HOST).retry);
    EXPECT_STREQ(AuthorizeWording::READ_TEXT, AuthorizeWording::wordingFor(HOST).read);
}

TEST(AccessTableTest, EveryLinuxMissingPrivilegeHasARow)
{
    // A privilege the notice can't explain would leave its loss unexplained: every bit has a row.
    for (const std::uint8_t bit : {Platform::MissingPrivilege::READ_OTHERS,
                                   Platform::MissingPrivilege::SIGNAL_OTHERS,
                                   Platform::MissingPrivilege::RAISE_PRIORITY,
                                   Platform::MissingPrivilege::PROCESSES_HIDDEN})
    {
        const auto rows = AccessTable::rowsFor(AccessPlatform::Linux);
        EXPECT_NE(std::ranges::find(rows, bit, &AccessTable::Row::missingPrivilege), rows.end()) << "no row for bit " << int{bit};
    }
}

TEST(AccessTableTest, EveryRowSaysWhatEachSessionGetsAndTheTableEndsWithWhatIsNeverAllowed)
{
    for (const AccessPlatform platform : PLATFORMS)
    {
        const auto rows = AccessTable::rowsFor(platform);
        EXPECT_FALSE(rows.empty());
        for (const AccessTable::Row& row : rows)
        {
            EXPECT_FALSE(row.item.empty());
            EXPECT_FALSE(row.standard.empty()) << row.item;
            EXPECT_FALSE(row.administrator.empty()) << row.item;
            EXPECT_NE(row.standard, row.administrator) << row.item << ": a row where Administrator adds nothing";
        }
        EXPECT_TRUE(AccessTable::neverRowFor(platform).starts_with("Never, even as Administrator"));
    }
}

TEST(AccessTableTest, WindowsRowsNeedNoHelperAndExplainNoLinuxBit)
{
    // Windows' Administrator is TaskSmack restarted elevated: no row waits on the helper, and its
    // missingPrivileges is always 0.
    for (const AccessTable::Row& row : AccessTable::rowsFor(AccessPlatform::Windows))
    {
        EXPECT_FALSE(row.needsHelperCoverage) << row.item;
        EXPECT_EQ(row.missingPrivilege, 0U) << row.item;
    }
}

} // namespace
} // namespace App::AccessChoice
