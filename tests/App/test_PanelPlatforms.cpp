/// @file test_PanelPlatforms.cpp
/// @brief What PanelPlatforms makes for a real run: the sign-in entry (#3093), the probes no helper reads
/// through since protocol v23 (#3580), the crash history's and System Information's, and the memory map reader,
/// which Windows' helper reads through again since v25 (#3665).

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

// #3580: the crash history's probe and System Information's are the platform's, with no helper decorator (#2416,
// #2310 went through one): making them asks the shared broker nothing.
TEST(PanelPlatformsTest, TheCrashHistoryAndSystemInformationAskNoHelper)
{
    const auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/true);
    Detail::setSharedPrivilegedBrokerForTesting(broker);
    const std::shared_ptr<Platform::ISystemInfoProbe> crashHistory = makeCrashHistoryProbe(nullptr);
    const std::unique_ptr<Platform::ISystemInfoProbe> systemInfo = makeSystemInfoPanelProbe(nullptr);
    Detail::setSharedPrivilegedBrokerForTesting(nullptr);

    EXPECT_NE(crashHistory, nullptr);
    EXPECT_NE(systemInfo, nullptr);
    EXPECT_EQ(broker->connectCount(), 0);
    EXPECT_TRUE(broker->requests().empty());
}

// #3665 (v25): a real run's memory map reader goes through the shared broker, so a refused read offers
// "Authenticate to read" where the helper reads other users' processes. Making it asks the broker nothing.
TEST(PanelPlatformsTest, TheMemoryMapReaderReadsThroughTheHelper)
{
    const auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/false);
    broker->setReadsOtherUsers(true);
    Detail::setSharedPrivilegedBrokerForTesting(broker);
    const std::unique_ptr<Platform::IProcessMemoryMapReader> memoryMap = makeProcessMemoryMapReader(nullptr);
    Detail::setSharedPrivilegedBrokerForTesting(nullptr);

    ASSERT_NE(memoryMap, nullptr);
    if (memoryMap->hasMemoryMap())
    {
        EXPECT_TRUE(memoryMap->canReadAuthorized());
        broker->setReadsOtherUsers(false); // a helper that runs as another account (#2769): no prompt
        EXPECT_FALSE(memoryMap->canReadAuthorized());
    }
    EXPECT_EQ(broker->connectCount(), 0);
    EXPECT_TRUE(broker->requests().empty());
}

} // namespace
} // namespace App
