/// @file test_ElevationNoticeText.cpp
/// @brief Tests for the startup privilege notice's body text (App::ElevationNoticeText)

#include "App/ElevationNoticeText.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace App::ElevationNoticeText
{
namespace
{

namespace Missing = Platform::MissingPrivilege;

constexpr std::uint8_t ALL = Missing::READ_OTHERS | Missing::SIGNAL_OTHERS | Missing::RAISE_PRIORITY | Missing::PROCESSES_HIDDEN;

TEST(ElevationNoticeTextTest, LinuxNoticeNamesEveryValueUnavailableForOtherUsers)
{
    // #1287: without CAP_DAC_READ_SEARCH + CAP_SYS_PTRACE (which root normally has), another user's
    // process shows N/A for network usage too (it is attributed through the process's /proc/[pid]/fd
    // links), but the notice named only FDs and I/O. The values are worded as possibly unavailable:
    // with CAP_DAC_READ_SEARCH alone, FD counts still come back.
    const std::string text = linuxText(Missing::READ_OTHERS);
    EXPECT_TRUE(text.contains("File descriptor counts"));
    EXPECT_TRUE(text.contains("I/O statistics"));
    EXPECT_TRUE(text.contains("network"));
    EXPECT_TRUE(text.contains("other users"));
    EXPECT_TRUE(text.contains("may be unavailable"));
    EXPECT_TRUE(text.contains("sudo TaskSmack"));
    // Root with dropped capabilities also gets the notice, and sudo can't help it: the capabilities
    // themselves must be named, and the text must not claim the process lacks elevation.
    EXPECT_TRUE(text.contains("CAP_DAC_READ_SEARCH"));
    EXPECT_TRUE(text.contains("CAP_SYS_PTRACE"));
    EXPECT_TRUE(text.contains("root alone isn't enough"));
    EXPECT_FALSE(text.contains("without elevated privileges"));
    // Other users' processes are still listed with their basic fields; only complete data is missing.
    EXPECT_TRUE(text.contains("complete data"));
    // Only what is missing: nothing about signals, priority or hidepid here.
    EXPECT_FALSE(text.contains("CAP_KILL"));
    EXPECT_FALSE(text.contains("CAP_SYS_NICE"));
    EXPECT_FALSE(text.contains("hidepid"));
}

TEST(ElevationNoticeTextTest, LinuxNoticeSaysWhatEachMissingPermissionBlocks)
{
    // #2303: the actions on other users' processes, and hidepid hiding them, have their own permissions.
    EXPECT_TRUE(linuxText(Missing::SIGNAL_OTHERS).contains("CAP_KILL"));
    EXPECT_TRUE(linuxText(Missing::SIGNAL_OTHERS).contains("act on"));
    EXPECT_TRUE(linuxText(Missing::RAISE_PRIORITY).contains("CAP_SYS_NICE"));
    EXPECT_TRUE(linuxText(Missing::RAISE_PRIORITY).contains("Realtime I/O class"));
    EXPECT_TRUE(linuxText(Missing::PROCESSES_HIDDEN).contains("hidepid"));
    EXPECT_TRUE(linuxText(Missing::PROCESSES_HIDDEN).contains("gid="));
    EXPECT_FALSE(linuxText(Missing::PROCESSES_HIDDEN).contains("File descriptor counts"));

    const std::string all = linuxText(ALL);
    for (const std::string_view needle : {"CAP_DAC_READ_SEARCH", "CAP_KILL", "CAP_SYS_NICE", "hidepid", "sudo TaskSmack"})
    {
        EXPECT_TRUE(all.contains(needle)) << needle;
    }
}

TEST(ElevationNoticeTextTest, UnknownDetailsReadAsTheReadAccessNotice)
{
    EXPECT_EQ(linuxText(0), linuxText(Missing::READ_OTHERS));
    // Read access alone missing: the notice's original text, word for word (its layout is tested for it).
    EXPECT_EQ(linuxText(Missing::READ_OTHERS),
              "TaskSmack can't read complete data for other users' processes.\n\n"
              "File descriptor counts, I/O statistics and/or network usage may be unavailable for processes "
              "owned by other users.\n\n"
              "For complete data, run it as root (sudo TaskSmack) or grant it CAP_DAC_READ_SEARCH and CAP_SYS_PTRACE. "
              "In a container or service that drops capabilities, allow those two there; root alone isn't enough.");
    EXPECT_TRUE(linuxText(Missing::SIGNAL_OTHERS)
                    .contains("grant it CAP_KILL. In a container or service that drops "
                              "capabilities, allow it there"));
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
    const std::array<std::string, 4> texts{linuxText(Missing::READ_OTHERS), linuxText(ALL), std::string(WINDOWS), std::string(OTHER)};
    for (const std::string_view text : texts)
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
    EXPECT_EQ(forCurrentPlatform(Missing::SIGNAL_OTHERS), linuxText(Missing::SIGNAL_OTHERS));
#elif defined(_WIN32)
    EXPECT_EQ(forCurrentPlatform(Missing::SIGNAL_OTHERS), WINDOWS);
#else
    EXPECT_EQ(forCurrentPlatform(), OTHER);
#endif
}

} // namespace
} // namespace App::ElevationNoticeText
