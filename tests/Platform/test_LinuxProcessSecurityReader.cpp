/// @file test_LinuxProcessSecurityReader.cpp
/// @brief Platform::LinuxProcessSecurityReader (#1526): a synthetic /proc tree read through the procRoot
/// seam (the identity check, a reused PID, a missing process, optional label and cgroup files), and the
/// real /proc/self.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/Linux/LinuxProcessSecurityReader.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 100, .startTimeTicks = 4242};

class LinuxProcessSecurityReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::create_directories(m_Proc.path / "100" / "attr");
        write("stat", "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n");
        write("status",
              "Name:\tserver\n"
              "Uid:\t0\t0\t0\t0\n"
              "Gid:\t0\t0\t0\t0\n"
              "Groups:\t0\n"
              "CapInh:\t0000000000000000\n"
              "CapPrm:\t0000000000003000\n"
              "CapEff:\t0000000000003000\n"
              "CapBnd:\t000001ffffffffff\n"
              "CapAmb:\t0000000000000000\n"
              "NoNewPrivs:\t1\n"
              "Seccomp:\t2\n");
        write("attr/current", "system_u:system_r:httpd_t:s0\n");
        write("cgroup", "0::/system.slice/server.service\n");
    }

    void write(const char* name, std::string_view contents) const
    {
        std::ofstream(m_Proc.path / "100" / name) << contents;
    }

    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_security"};
};

TEST_F(LinuxProcessSecurityReaderTest, ReadsTheProcessesSecurityContext)
{
    LinuxProcessSecurityReader reader(m_Proc.path.string());
    EXPECT_TRUE(reader.hasSecurity());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    const ProcessSecurity& security = result.security;
    ASSERT_TRUE(security.users.has_value());
    EXPECT_EQ(security.users.value_or(SecurityIdSet{}).effective.id, 0U);
    EXPECT_EQ(security.users.value_or(SecurityIdSet{}).effective.name, "root"); // uid 0 is root in any passwd database
    ASSERT_TRUE(security.groups.has_value());
    EXPECT_EQ(security.groups.value_or(SecurityIdSet{}).real.id, 0U);
    ASSERT_EQ(security.supplementaryGroups.size(), 1U);
    EXPECT_EQ(security.capabilities.effective, std::optional<std::uint64_t>(0x3000));
    EXPECT_EQ(security.capabilities.bounding, std::optional<std::uint64_t>(0x1ffffffffff));
    EXPECT_EQ(security.noNewPrivileges, std::optional<bool>(true));
    EXPECT_EQ(security.seccomp, std::optional<SeccompMode>(SeccompMode::Filter));
    EXPECT_EQ(security.securityLabel, "system_u:system_r:httpd_t:s0");
    EXPECT_EQ(security.controlGroup, "/system.slice/server.service");
}

TEST_F(LinuxProcessSecurityReaderTest, AMissingLabelOrCgroupLeavesThemEmpty)
{
    std::filesystem::remove(m_Proc.path / "100" / "attr" / "current");
    std::filesystem::remove(m_Proc.path / "100" / "cgroup");
    LinuxProcessSecurityReader reader(m_Proc.path.string());
    const SecurityReadResult result = reader.readSecurity(TARGET);
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    EXPECT_TRUE(result.security.securityLabel.empty());
    EXPECT_TRUE(result.security.controlGroup.empty());
    EXPECT_TRUE(result.security.users.has_value());
}

TEST_F(LinuxProcessSecurityReaderTest, RefusesAReusedPidAnUnknownIdentityAndAMissingProcess)
{
    LinuxProcessSecurityReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readSecurity({.pid = 100, .startTimeTicks = 9999}).status, SecurityReadStatus::ProcessExited);
    EXPECT_EQ(reader.readSecurity({.pid = 100, .startTimeTicks = 0}).status, SecurityReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readSecurity({.pid = 101, .startTimeTicks = 4242}).status, SecurityReadStatus::ProcessExited);
    EXPECT_EQ(reader.readSecurity({.pid = 0, .startTimeTicks = 4242}).status, SecurityReadStatus::ProcessExited);
}

TEST_F(LinuxProcessSecurityReaderTest, AnEmptyStatusFromAnExitingProcessReadsAsExited)
{
    write("status", "");
    write("stat", ""); // the process went away between the identity check and the re-check
    LinuxProcessSecurityReader reader(m_Proc.path.string());
    // The first identity check already fails on the empty stat: exited, never an empty "Ok".
    EXPECT_EQ(reader.readSecurity(TARGET).status, SecurityReadStatus::ProcessExited);
}

TEST(LinuxProcessSecurityReaderRealTest, ReadsThisProcess)
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(line);
    ASSERT_TRUE(startTicks.has_value());

    LinuxProcessSecurityReader reader;
    const SecurityReadResult result =
        reader.readSecurity({.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = startTicks.value_or(0)});
    ASSERT_EQ(result.status, SecurityReadStatus::Ok);
    ASSERT_TRUE(result.security.users.has_value());
    EXPECT_EQ(result.security.users.value_or(SecurityIdSet{}).effective.id, static_cast<std::uint32_t>(::geteuid()));
    ASSERT_TRUE(result.security.groups.has_value());
    EXPECT_EQ(result.security.groups.value_or(SecurityIdSet{}).effective.id, static_cast<std::uint32_t>(::getegid()));
    EXPECT_TRUE(result.security.capabilities.effective.has_value());
    EXPECT_TRUE(result.security.capabilities.bounding.has_value());
}

} // namespace
} // namespace Platform
