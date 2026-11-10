/// @file test_PanelPlatforms.cpp
/// @brief What PanelPlatforms makes for a real run: the sign-in entry (#3093), and the probes no helper reads
/// through since protocol v23 (#3580): the crash history's, System Information's and the memory map's.

#include "App/PanelPlatforms.h"
#include "Mocks/FakeBroker.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <memory>

namespace App
{
namespace
{

// #3093 part 2: the sign-in entry is the platform's (an XDG autostart entry on Linux, #3254), and none while a
// test hook is set, so an unattended run never writes it. Making it reads and writes nothing.
TEST(PanelPlatformsTest, TheSignInEntryIsThePlatformsAndNoneUnderATestHook)
{
    EXPECT_EQ(makeStartAtLoginEntry(nullptr, true), nullptr);
    EXPECT_NE(makeStartAtLoginEntry(nullptr, false), nullptr);
}

// #3580: the crash history's probe, System Information's and Process Details' memory map reader are the platform's,
// with no helper decorator (#2416, #2310, #3089 went through one): making them asks the shared broker nothing.
TEST(PanelPlatformsTest, TheCrashHistorySystemInformationAndMemoryMapAskNoHelper)
{
    const auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/true);
    Detail::setSharedPrivilegedBrokerForTesting(broker);
    const std::shared_ptr<Platform::ISystemInfoProbe> crashHistory = makeCrashHistoryProbe(nullptr);
    const std::unique_ptr<Platform::ISystemInfoProbe> systemInfo = makeSystemInfoPanelProbe(nullptr);
    const std::unique_ptr<Platform::IProcessMemoryMapReader> memoryMap = makeProcessMemoryMapReader(nullptr);
    Detail::setSharedPrivilegedBrokerForTesting(nullptr);

    EXPECT_NE(crashHistory, nullptr);
    EXPECT_NE(systemInfo, nullptr);
    EXPECT_NE(memoryMap, nullptr);
    EXPECT_EQ(broker->connectCount(), 0);
    EXPECT_TRUE(broker->requests().empty());
}

} // namespace
} // namespace App
