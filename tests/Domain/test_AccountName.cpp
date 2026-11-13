/// @file test_AccountName.cpp
/// @brief A process owner's account as the Processes table and Process Details write it and the filter
/// finds it (#3328): "PC\alice" where the domain is known (Windows), the user name alone where not (Linux).

#include "Domain/AccountName.h"

#include <gtest/gtest.h>

namespace Domain::AccountName
{
namespace
{

TEST(AccountNameTest, QualifiedWritesTheDomainBeforeTheUser)
{
    EXPECT_EQ(qualified("PC", "alice"), "PC\\alice");
    EXPECT_EQ(qualified("NT AUTHORITY", "SYSTEM"), "NT AUTHORITY\\SYSTEM");
    EXPECT_EQ(qualified("CORP", "Alice"), "CORP\\Alice"); // Spelled as the probe read it
}

TEST(AccountNameTest, QualifiedIsTheUserAloneWithoutADomain)
{
    EXPECT_EQ(qualified("", "alice"), "alice"); // Linux, or a Windows owner whose domain wasn't read
    EXPECT_EQ(qualified("", ""), "");
    EXPECT_EQ(qualified("PC", ""), ""); // No user read: no account to write, whatever the domain
}

TEST(AccountNameTest, MatchesTheShortNameAndTheDomainIgnoringCase)
{
    EXPECT_TRUE(matches("PC", "alice", "alice"));
    EXPECT_TRUE(matches("PC", "alice", "LIC"));
    EXPECT_TRUE(matches("CORP", "alice", "corp"));
    EXPECT_TRUE(matches("", "alice", "Alice")); // Linux: the user name alone
    EXPECT_TRUE(matches("PC", "alice", ""));    // An empty term is in every account
    EXPECT_FALSE(matches("PC", "alice", "bob"));
    EXPECT_FALSE(matches("", "alice", "pc"));
}

TEST(AccountNameTest, MatchesASpanAcrossTheSeparator)
{
    EXPECT_TRUE(matches("CORP", "alice", "CORP\\alice"));
    EXPECT_TRUE(matches("CORP", "alice", "corp\\ALICE"));
    EXPECT_TRUE(matches("CORP", "alice", "rp\\al")); // The end of the domain, the start of the user
    EXPECT_TRUE(matches("CORP", "alice", "\\a"));
    EXPECT_TRUE(matches("CORP", "alice", "p\\"));
    EXPECT_TRUE(matches("CORP", "alice", "\\"));
    EXPECT_FALSE(matches("PC", "alice", "CORP\\alice"));  // Another account
    EXPECT_FALSE(matches("CORP", "alice", "co\\alice"));  // "co" doesn't end the domain
    EXPECT_FALSE(matches("CORP", "alice", "corp\\lice")); // "lice" doesn't start the user name
    EXPECT_FALSE(matches("CORP", "alice", "corp\\alicex"));
    EXPECT_FALSE(matches("CORP", "alice", "xcorp\\alice"));
    EXPECT_FALSE(matches("CORP", "alice", "corp\\al\\ice")); // Only one separator is written
    EXPECT_FALSE(matches("", "alice", "\\alice"));           // No domain, so no separator
}

TEST(AccountNameTest, NothingMatchesAnOwnerWhoseUserWasNotRead)
{
    EXPECT_FALSE(matches("PC", "", "pc"));
    EXPECT_FALSE(matches("", "", ""));
}

// Evaluated by the compiler too: the Processes filter calls it on every row of every rebuild.
static_assert(matches("PC", "alice", "pc\\a"));
static_assert(!matches("PC", "alice", "pc\\b"));

} // namespace
} // namespace Domain::AccountName
