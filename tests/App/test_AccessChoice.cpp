/// @file test_AccessChoice.cpp
/// @brief Standard or Administrator (#2728): each platform's wording, and the access table the Standard note
/// expands.

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

TEST(AccessTableTest, WindowsRowsExplainNoLinuxBit)
{
    // Windows' Administrator is TaskSmack restarted elevated: its missingPrivileges is always 0.
    for (const AccessTable::Row& row : AccessTable::rowsFor(AccessPlatform::Windows))
    {
        EXPECT_EQ(row.missingPrivilege, 0U) << row.item;
    }
}

// #3580: a Linux Standard session is never elevated after it starts, so no row offers a password prompt or a
// helper, and Administrator (TaskSmack started as root) gets every row.
TEST(AccessTableTest, LinuxRowsOfferNoPromptAndNoHelper)
{
    for (const AccessTable::Row& row : AccessTable::rowsFor(AccessPlatform::Linux))
    {
        for (const std::string_view cell : {row.standard, row.administrator})
        {
            EXPECT_FALSE(cell.contains("Authenticate")) << row.item;
            EXPECT_FALSE(cell.contains("helper")) << row.item;
            EXPECT_FALSE(cell.contains("asks")) << row.item;
        }
    }
}

} // namespace
} // namespace App::AccessChoice
