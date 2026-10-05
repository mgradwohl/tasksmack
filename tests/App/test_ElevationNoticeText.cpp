/// @file test_ElevationNoticeText.cpp
/// @brief Tests for the startup privilege notice's body text (App::ElevationNoticeText)

#include "App/ElevationNoticeText.h"

#include <gtest/gtest.h>

#include <string_view>

namespace App::ElevationNoticeText
{
namespace
{

TEST(ElevationNoticeTextTest, LinuxNoticeNamesEveryValueUnavailableForOtherUsers)
{
    // #1287: without root, another user's process shows N/A for network usage too (it is attributed
    // through the process's /proc/[pid]/fd links), but the notice named only FDs and I/O.
    EXPECT_TRUE(LINUX.contains("File descriptor counts"));
    EXPECT_TRUE(LINUX.contains("I/O statistics"));
    EXPECT_TRUE(LINUX.contains("network"));
    EXPECT_TRUE(LINUX.contains("other users"));
    EXPECT_TRUE(LINUX.contains("sudo TaskSmack"));
}

TEST(ElevationNoticeTextTest, WindowsNoticeNamesNetwork)
{
    EXPECT_TRUE(WINDOWS.contains("network"));
    EXPECT_TRUE(WINDOWS.contains("Administrator"));
}

TEST(ElevationNoticeTextTest, CurrentPlatformPicksItsOwnText)
{
#ifdef __linux__
    EXPECT_EQ(forCurrentPlatform(), LINUX);
#elif defined(_WIN32)
    EXPECT_EQ(forCurrentPlatform(), WINDOWS);
#else
    EXPECT_EQ(forCurrentPlatform(), OTHER);
#endif
}

} // namespace
} // namespace App::ElevationNoticeText
