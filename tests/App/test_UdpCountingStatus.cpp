// What Count UDP traffic per process is doing, from the Processes panel to Settings (#3366).

#include "App/UdpCountingStatus.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace App
