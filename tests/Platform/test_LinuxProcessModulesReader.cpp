/// @file test_LinuxProcessModulesReader.cpp
/// @brief Platform::LinuxProcessModulesReader (#802): a synthetic /proc tree read through the procRoot
/// seam (the identity check, a reused PID, a missing process), and the real /proc/self.

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "Platform/Linux/LinuxProcessModulesReader.h"
#include "Platform/Linux/ProcParsing.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
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

class LinuxProcessModulesReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::filesystem::create_directories(m_Proc.path / "100");
        write("stat", "100 (server) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n");
        write("maps",
              "55d0c1a00000-55d0c1a02000 r-xp 00000000 08:01 1311 /usr/bin/server\n"
              "7f3a1c028000-7f3a1c1bd000 r-xp 00028000 08:01 1835 /usr/lib/libc.so.6\n"
              "7ffd5a1f0000-7ffd5a211000 rw-p 00000000 00:00 0 [stack]\n");
    }

    void write(const char* name, std::string_view contents) const
    {
        std::ofstream(m_Proc.path / "100" / name) << contents;
    }

    TestSupport::ScopedTempDir m_Proc{"ts_test_proc_modules"};
};

TEST_F(LinuxProcessModulesReaderTest, ReadsTheProcessesModules)
{
    LinuxProcessModulesReader reader(m_Proc.path.string());
    EXPECT_TRUE(reader.hasModules());
    const ModulesReadResult result = reader.readModules(TARGET);
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    ASSERT_EQ(result.modules.size(), 2U);
    EXPECT_EQ(result.modules[0].path, "/usr/bin/server");
    EXPECT_EQ(result.modules[1].path, "/usr/lib/libc.so.6");
}

TEST_F(LinuxProcessModulesReaderTest, RefusesAReusedPidAnUnknownIdentityAndAMissingProcess)
{
    LinuxProcessModulesReader reader(m_Proc.path.string());
    EXPECT_EQ(reader.readModules({.pid = 100, .startTimeTicks = 9999}).status, ModulesReadStatus::ProcessExited);
    EXPECT_EQ(reader.readModules({.pid = 100, .startTimeTicks = 0}).status, ModulesReadStatus::IdentityUnknown);
    EXPECT_EQ(reader.readModules({.pid = 101, .startTimeTicks = 4242}).status, ModulesReadStatus::ProcessExited);
}

TEST(LinuxProcessModulesReaderRealTest, ReadsThisProcess)
{
    std::ifstream stat("/proc/self/stat");
    const std::string line((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(line);
    ASSERT_TRUE(startTicks.has_value());

    LinuxProcessModulesReader reader;
    const ModulesReadResult result =
        reader.readModules({.pid = static_cast<std::int32_t>(::getpid()), .startTimeTicks = startTicks.value_or(0)});
    ASSERT_EQ(result.status, ModulesReadStatus::Ok);
    const std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    EXPECT_TRUE(std::ranges::any_of(result.modules, [&self](const ProcessModule& m) { return m.path == self; })) << self;
}

} // namespace
} // namespace Platform
