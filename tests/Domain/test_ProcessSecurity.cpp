/// @file test_ProcessSecurity.cpp
/// @brief Domain::ProcessSecurity (#1526): capability names and the Security section's display text.

#include "Domain/ProcessSecurity.h"
#include "Platform/IProcessSecurity.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Domain::ProcessSecurity
{
namespace
{

TEST(ProcessSecurityTest, NamesCapabilitiesByBit)
{
    EXPECT_EQ(capabilityName(0), "CAP_CHOWN");
    EXPECT_EQ(capabilityName(21), "CAP_SYS_ADMIN");
    EXPECT_EQ(capabilityName(40), "CAP_CHECKPOINT_RESTORE");
    EXPECT_EQ(capabilityName(41), "cap_41"); // a newer kernel's bit, named as capsh prints it
    EXPECT_EQ(capabilityNames((std::uint64_t{1} << 10) | (std::uint64_t{1} << 13)),
              (std::vector<std::string>{"CAP_NET_BIND_SERVICE", "CAP_NET_RAW"}));
    EXPECT_TRUE(capabilityNames(0).empty());
}

TEST(ProcessSecurityTest, CapabilitySetTextSummarisesTheFullSet)
{
    EXPECT_EQ(capabilitySetText(0), "none");
    EXPECT_EQ(capabilitySetText(0x1ffffffffffULL), "all (41)");
    EXPECT_EQ(capabilitySetText(0x3ffffffffffULL), "all (41 + 1 unknown)");
    EXPECT_EQ(capabilitySetText(std::uint64_t{1} << 21), "CAP_SYS_ADMIN");
    EXPECT_EQ(capabilitySetText((std::uint64_t{1} << 12) | (std::uint64_t{1} << 13)), "CAP_NET_ADMIN, CAP_NET_RAW");
}

TEST(ProcessSecurityTest, IdSetTextShowsOnlyTheIdsThatDiffer)
{
    const Platform::SecurityPrincipal matt{.id = 1000, .name = "matt"};
    const Platform::SecurityPrincipal root{.id = 0, .name = "root"};
    EXPECT_EQ(idSetText({.real = matt, .effective = matt, .saved = matt, .filesystem = matt}), "1000 (matt)");
    EXPECT_EQ(idSetText({.real = matt, .effective = root, .saved = matt, .filesystem = root}),
              "0 (root), real 1000 (matt), saved 1000 (matt)");
    EXPECT_EQ(principalText({.id = 4242, .name = {}}), "4242");
}

TEST(ProcessSecurityTest, GroupListAndSeccompText)
{
    EXPECT_EQ(groupListText({}), "none");
    EXPECT_EQ(groupListText({{.id = 4, .name = "adm"}, {.id = 27, .name = {}}}), "4 (adm), 27");
    EXPECT_EQ(seccompText(Platform::SeccompMode::Disabled), "Disabled");
    EXPECT_EQ(seccompText(Platform::SeccompMode::Strict), "Strict");
    EXPECT_EQ(seccompText(Platform::SeccompMode::Filter), "Filter");
}

} // namespace
} // namespace Domain::ProcessSecurity
