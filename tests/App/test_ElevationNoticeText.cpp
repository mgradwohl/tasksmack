/// @file test_ElevationNoticeText.cpp
/// @brief Tests for the startup privilege notice's body text (App::ElevationNoticeText)

#include "App/ElevationNoticeText.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace App::ElevationNoticeText
{
namespace
{

TEST(ElevationNoticeTextTest, LinuxNoticeNamesEveryValueUnavailableForOtherUsers)
{
    // #1287: without CAP_DAC_READ_SEARCH + CAP_SYS_PTRACE (which root normally has), another user's
    // process shows N/A for network usage too (it is attributed through the process's /proc/[pid]/fd
    // links), but the notice named only FDs and I/O. The values are worded as possibly unavailable:
    // with CAP_DAC_READ_SEARCH alone, FD counts still come back.
    EXPECT_TRUE(LINUX.contains("File descriptor counts"));
    EXPECT_TRUE(LINUX.contains("I/O statistics"));
    EXPECT_TRUE(LINUX.contains("network"));
    EXPECT_TRUE(LINUX.contains("other users"));
    EXPECT_TRUE(LINUX.contains("may be unavailable"));
    EXPECT_TRUE(LINUX.contains("sudo TaskSmack"));
    // Root with dropped capabilities also gets the notice, and sudo can't help it: the capabilities
    // themselves must be named, and the text must not claim the process lacks elevation.
    EXPECT_TRUE(LINUX.contains("CAP_DAC_READ_SEARCH"));
    EXPECT_TRUE(LINUX.contains("CAP_SYS_PTRACE"));
    EXPECT_TRUE(LINUX.contains("root alone isn't enough"));
    EXPECT_FALSE(LINUX.contains("without elevated privileges"));
    // Other users' processes are still listed with their basic fields; only complete data is missing.
    EXPECT_TRUE(LINUX.contains("complete data"));
}

TEST(ElevationNoticeTextTest, WindowsNoticeNamesNetwork)
{
    EXPECT_TRUE(WINDOWS.contains("network"));
    EXPECT_TRUE(WINDOWS.contains("Administrator"));
}

// The notice wraps its text to the dialog, so a line break inside a paragraph cut a line short beside
// the wrapped ones (#1200). Breaks may only separate paragraphs: every '\n' is half of a "\n\n".
TEST(ElevationNoticeTextTest, LineBreaksOnlySeparateParagraphs)
{
    for (const std::string_view text : {LINUX, WINDOWS, OTHER})
    {
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '\n')
            {
                continue;
            }
            const bool pairedBefore = (i > 0) && text[i - 1] == '\n';
            const bool pairedAfter = (i + 1 < text.size()) && text[i + 1] == '\n';
            EXPECT_TRUE(pairedBefore || pairedAfter) << "lone line break at " << i << " in: " << text;
        }
        EXPECT_FALSE(text.contains("\n\n\n"));
    }
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
