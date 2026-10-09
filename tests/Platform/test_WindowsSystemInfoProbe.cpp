/// @file test_WindowsSystemInfoProbe.cpp
/// @brief The real WindowsSystemInfoProbe (#1512), smoke-tested: it reports an edition and a build,
/// and the session facts any signed-in test run has.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsSystemInfoProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace Platform
{
namespace
{

TEST(WindowsSystemInfoProbeTest, ReadsAnEditionAndABuild)
{
    WindowsSystemInfoProbe probe;
    ASSERT_TRUE(probe.capabilities().hasOs);

    const OsInfo info = probe.readOs();
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.name.starts_with("Windows")) << info.name;
    EXPECT_FALSE(info.build.empty());
    EXPECT_NE(info.build.find('.'), std::string::npos) << info.build; // CurrentBuild.UBR
    EXPECT_FALSE(info.architecture.empty());
    EXPECT_FALSE(info.computerName.empty());
    EXPECT_FALSE(info.locale.empty());
    EXPECT_FALSE(info.timeZone.empty());
    EXPECT_TRUE(info.utcOffsetMinutes.has_value());
    EXPECT_FALSE(info.systemDirectory.empty());
    EXPECT_FALSE(info.windowsDirectory.empty());

    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    EXPECT_GT(info.bootUnixSeconds, 0U);
    EXPECT_LE(info.bootUnixSeconds, now);
}

} // namespace
} // namespace Platform
