/// @file test_ProcStatusSecurityParser.cpp
/// @brief Platform::ProcStatusSecurity (#1526): the security fields of /proc/[pid]/status, the LSM label
/// in attr/current and the cgroup path, including missing and malformed input.

#include "Platform/IProcessSecurity.h"
#include "Platform/Linux/ProcStatusSecurityParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Platform::ProcStatusSecurity
{
namespace
{

using namespace std::string_view_literals;

constexpr std::string_view STATUS = "Name:\tbash\n"
                                    "Umask:\t0022\n"
                                    "State:\tS (sleeping)\n"
                                    "Uid:\t1000\t0\t1000\t0\n"
                                    "Gid:\t1000\t1000\t1000\t1000\n"
                                    "FDSize:\t256\n"
                                    "Groups:\t4 24 27 1000 \n"
                                    "CapInh:\t0000000000000000\n"
                                    "CapPrm:\t0000000000000000\n"
                                    "CapEff:\t0000000000000400\n"
                                    "CapBnd:\t000001ffffffffff\n"
                                    "CapAmb:\t0000000000000000\n"
                                    "NoNewPrivs:\t0\n"
                                    "Seccomp:\t0\n"
                                    "Seccomp_filters:\t0\n";

TEST(ProcStatusSecurityParserTest, ParsesEveryField)
{
    const ProcessSecurity security = parseStatus(STATUS);
    ASSERT_TRUE(security.users.has_value());
    EXPECT_EQ(security.users.value_or(SecurityIdSet{}).real.id, 1000U);
    EXPECT_EQ(security.users.value_or(SecurityIdSet{}).effective.id, 0U); // a setuid program
    EXPECT_EQ(security.users.value_or(SecurityIdSet{}).saved.id, 1000U);
    EXPECT_EQ(security.users.value_or(SecurityIdSet{}).filesystem.id, 0U);
    ASSERT_TRUE(security.groups.has_value());
    EXPECT_EQ(security.groups.value_or(SecurityIdSet{}).effective.id, 1000U);
    ASSERT_EQ(security.supplementaryGroups.size(), 4U);
    EXPECT_EQ(security.supplementaryGroups[0].id, 4U);
    EXPECT_EQ(security.supplementaryGroups[3].id, 1000U);
    EXPECT_EQ(security.capabilities.effective, std::optional<std::uint64_t>(0x400));
    EXPECT_EQ(security.capabilities.permitted, std::optional<std::uint64_t>(0));
    EXPECT_EQ(security.capabilities.bounding, std::optional<std::uint64_t>(0x1ffffffffff));
    EXPECT_EQ(security.capabilities.ambient, std::optional<std::uint64_t>(0));
    EXPECT_EQ(security.noNewPrivileges, std::optional<bool>(false));
    EXPECT_EQ(security.seccomp, std::optional<SeccompMode>(SeccompMode::Disabled));
}

TEST(ProcStatusSecurityParserTest, SeccompIsNotConfusedWithSeccompFilters)
{
    // "Seccomp_filters:" starts with "Seccomp" but is another field; only "Seccomp:" is the mode.
    const ProcessSecurity security = parseStatus("Seccomp_filters:\t2\nSeccomp:\t1\n");
    EXPECT_EQ(security.seccomp, std::optional<SeccompMode>(SeccompMode::Strict));
    EXPECT_FALSE(parseStatus("Seccomp_filters:\t2\n").seccomp.has_value());
}

TEST(ProcStatusSecurityParserTest, MissingAndMalformedFieldsAreLeftEmpty)
{
    const ProcessSecurity empty = parseStatus("");
    EXPECT_FALSE(empty.users.has_value());
    EXPECT_FALSE(empty.groups.has_value());
    EXPECT_TRUE(empty.supplementaryGroups.empty());
    EXPECT_FALSE(empty.capabilities.effective.has_value());
    EXPECT_FALSE(empty.noNewPrivileges.has_value());
    EXPECT_FALSE(empty.seccomp.has_value());

    const ProcessSecurity bad = parseStatus("Uid:\t1000\t1000\n"       // three of the four IDs missing
                                            "Gid:\t1000 x 1000 1000\n" // not a number
                                            "Groups:\t4 -1\n"
                                            "CapEff:\tzz\n"
                                            "CapAmb:\t\n" // an old kernel's empty value
                                            "NoNewPrivs:\t7\n"
                                            "Seccomp:\t9\n");
    EXPECT_FALSE(bad.users.has_value());
    EXPECT_FALSE(bad.groups.has_value());
    EXPECT_TRUE(bad.supplementaryGroups.empty());
    EXPECT_FALSE(bad.capabilities.effective.has_value());
    EXPECT_FALSE(bad.capabilities.ambient.has_value());
    EXPECT_FALSE(bad.noNewPrivileges.has_value());
    EXPECT_FALSE(bad.seccomp.has_value());
}

TEST(ProcStatusSecurityParserTest, AnEmptyGroupsLineIsNoGroups)
{
    const ProcessSecurity security = parseStatus("Groups:\t\nUid:\t0\t0\t0\t0\n");
    EXPECT_TRUE(security.supplementaryGroups.empty());
    EXPECT_TRUE(security.users.has_value());
}

TEST(ProcStatusSecurityParserTest, FieldValueMatchesWholeKeysOnly)
{
    EXPECT_EQ(fieldValue("CapEffX:\t1\nCapEff:\t2\n", "CapEff"), std::optional<std::string_view>("2"));
    EXPECT_FALSE(fieldValue("Uidx:\t1\n", "Uid").has_value());
    EXPECT_EQ(fieldValue("Uid:   1 2 3 4  \r\n", "Uid"), std::optional<std::string_view>("1 2 3 4"));
}

TEST(ProcStatusSecurityParserTest, SecurityLabelStopsAtNulOrNewline)
{
    EXPECT_EQ(parseSecurityLabel("unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023\n"),
              "unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023");
    EXPECT_EQ(parseSecurityLabel("/usr/bin/firefox (enforce)\0junk"sv), "/usr/bin/firefox (enforce)");
    EXPECT_EQ(parseSecurityLabel("unconfined"), "unconfined");
    EXPECT_EQ(parseSecurityLabel(""), "");
    EXPECT_EQ(parseSecurityLabel(" \n"), "");
}

TEST(ProcStatusSecurityParserTest, ControlGroupPrefersTheUnifiedHierarchy)
{
    EXPECT_EQ(parseControlGroup("0::/user.slice/user-1000.slice/session-2.scope\n"), "/user.slice/user-1000.slice/session-2.scope");
    // A hybrid system lists v1 hierarchies too; the v2 line still wins.
    EXPECT_EQ(parseControlGroup("12:cpu,cpuacct:/user.slice\n0::/user.slice/app.scope\n"), "/user.slice/app.scope");
    // v1 only: every line, joined.
    EXPECT_EQ(parseControlGroup("4:memory:/docker/abc\n2:cpu:/docker/abc\n"), "4:memory:/docker/abc; 2:cpu:/docker/abc");
    EXPECT_EQ(parseControlGroup(""), "");
}

} // namespace
} // namespace Platform::ProcStatusSecurity
