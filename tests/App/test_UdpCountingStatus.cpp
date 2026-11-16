// What Count UDP traffic per process is doing, from the Processes panel to Settings (#3366).

#include "App/UdpCountingStatus.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace App
{
namespace
{

TEST(UdpCountingStatusTest, TheLastPublishedStatusIsWhatSettingsReads)
{
    publishUdpCountingStatus({.delivering = false, .unsupported = true, .problem = "fexit: loading failed"});
    EXPECT_EQ(udpCountingStatus(), (UdpCountingStatus{.delivering = false, .unsupported = true, .problem = "fexit: loading failed"}));
    publishUdpCountingStatus({.delivering = true, .unsupported = false, .problem = {}});
    EXPECT_TRUE(udpCountingStatus().delivering);
    EXPECT_FALSE(udpCountingStatus().unsupported);
    EXPECT_TRUE(udpCountingStatus().problem.empty());
    publishUdpCountingStatus({}); // as a run starts: nothing known yet
}

// #3878: Settings reads the status every frame; it must copy (and allocate) only when the collector published a change.
TEST(UdpCountingStatusTest, AnUnchangedStatusIsNotCopiedAgain)
{
    const std::string problem = "Needs TaskSmack started as administrator: a long problem text";
    publishUdpCountingStatus({.delivering = false, .unsupported = false, .problem = problem});
    std::uint64_t seen = 0;
    UdpCountingStatus mine;
    ASSERT_TRUE(udpCountingStatusIfChanged(seen, mine));
    EXPECT_EQ(mine.problem, problem);

    // A marker no copy would leave alone: an unchanged status must not write into the caller's copy at all.
    mine.problem = "marker";
    EXPECT_FALSE(udpCountingStatusIfChanged(seen, mine));
    EXPECT_FALSE(udpCountingStatusIfChanged(seen, mine));
    EXPECT_EQ(mine.problem, "marker");

    // Publishing what is already held is not a change.
    publishUdpCountingStatus({.delivering = false, .unsupported = false, .problem = problem});
    EXPECT_FALSE(udpCountingStatusIfChanged(seen, mine));
    EXPECT_EQ(mine.problem, "marker");

    // A real change is copied once, then quiet again.
    publishUdpCountingStatus({.delivering = true, .unsupported = false, .problem = {}});
    EXPECT_TRUE(udpCountingStatusIfChanged(seen, mine));
    EXPECT_TRUE(mine.delivering);
    EXPECT_TRUE(mine.problem.empty());
    EXPECT_FALSE(udpCountingStatusIfChanged(seen, mine));
    publishUdpCountingStatus({});
}

// #3879: Settings' line follows what the collector says, on both platforms, rather than always saying it needs
// administrator.
TEST(UdpCountingLineTest, CountingIsSaidWhenItCounts)
{
    const UdpCountingLine line = udpCountingLine(true, true, false, {});
    EXPECT_EQ(line.text(), "Counting UDP.");
    EXPECT_FALSE(line.warning);
}

TEST(UdpCountingLineTest, NotAdministratorIsTheCollectorsProblemAsASentence)
{
    for (const bool on : {false, true})
    {
        const UdpCountingLine onWindows = udpCountingLine(on, false, false, "Needs TaskSmack running as administrator");
        EXPECT_EQ(onWindows.text(), "Needs TaskSmack running as administrator.") << on;
        EXPECT_FALSE(onWindows.warning);
        const UdpCountingLine onLinux =
            udpCountingLine(on, false, false, "Needs TaskSmack started as administrator: TaskSmack (Administrator), or sudo");
        EXPECT_EQ(onLinux.text(), "Needs TaskSmack started as administrator: TaskSmack (Administrator), or sudo.") << on;
    }
    EXPECT_EQ(udpCountingLine(true, false, false, "Already a sentence.").text(), "Already a sentence.") << "no second period";
}

TEST(UdpCountingLineTest, ATraceThatDidNotStartSaysWhy)
{
    const UdpCountingLine line = udpCountingLine(true, false, false, "Windows refused to start the network event trace (access denied)");
    EXPECT_EQ(line.text(), "Windows refused to start the network event trace (access denied).");
}

TEST(UdpCountingLineTest, ACounterThisSystemCantLoadIsAWarning)
{
    const UdpCountingLine line = udpCountingLine(false, false, true, "fexit: loading failed");
    EXPECT_EQ(line.text(), "This system can't count UDP per process: fexit: loading failed");
    EXPECT_TRUE(line.warning);
}

TEST(UdpCountingLineTest, AnAdministratorSessionWithNothingToReportSaysWhatTheSettingDoes)
{
    EXPECT_EQ(udpCountingLine(true, false, false, {}).text(), "On: UDP is counted from the first UDP traffic.");
    EXPECT_EQ(udpCountingLine(false, false, false, {}).text(), "Off: Net Sent and Net Received count TCP only.");
    EXPECT_EQ(udpCountingLine(false, true, false, {}).text(), "Off: Net Sent and Net Received count TCP only.")
        << "unticked before Save: off is what Save does";
}

} // namespace
} // namespace App
