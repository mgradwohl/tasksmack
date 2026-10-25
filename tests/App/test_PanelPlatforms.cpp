/// @file test_PanelPlatforms.cpp
/// @brief The crash history's probe (#2416): Process Details' crash history and System Information
/// publish to one Domain::CrashHistory, so once the helper runs, Process Details' own reads must bring
/// every user's crashes too, not take back the ones System Information's brokered read published.

#include "App/PanelPlatforms.h"
#include "Domain/CrashHistory.h"
#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <string>
#include <variant>

namespace App
{
namespace
{

/// A non-root Linux read: this user's crashes only, the other users' hidden.
class OwnCrashesOnlyProbe final : public Platform::ISystemInfoProbe
{
  public:
    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = true, .unavailableReason = {}, .wifiDetailsGated = false, .hasPublicIp = false};
    }
    [[nodiscard]] Platform::OsInfo readOs() override
    {
        return {};
    }
    [[nodiscard]] Platform::FirmwareInfo readFirmware() override
    {
        return {};
    }
    [[nodiscard]] Platform::MemoryModulesInfo readMemoryModules() override
    {
        return {};
    }
    [[nodiscard]] Platform::CommitPagingInfo readCommitPaging() override
    {
        return {};
    }
    [[nodiscard]] Platform::StorageInfo readStorage() override
    {
        return {};
    }
    [[nodiscard]] Platform::GraphicsInfo readGraphics() override
    {
        return {};
    }
    [[nodiscard]] Platform::PlatformSecurityInfo readPlatformSecurity() override
    {
        return {};
    }
    [[nodiscard]] Platform::SensorsInfo readSensors() override
    {
        return {};
    }
    [[nodiscard]] Platform::DevicesInfo readDevices() override
    {
        return {};
    }
    [[nodiscard]] Platform::DriversInfo readDrivers() override
    {
        return {};
    }
    [[nodiscard]] Platform::CrashesInfo readCrashes() override
    {
        Platform::CrashesInfo info;
        info.available = true;
        info.listed = true;
        info.journalRead = true;
        info.otherUsersHidden = true;
        Platform::CrashEvent own;
        own.application = "mine";
        info.events.push_back(own);
        return info;
    }
    [[nodiscard]] Platform::NetworkAdaptersInfo readNetworkAdapters(bool /*includeWifiDetails*/) override
    {
        return {};
    }
    [[nodiscard]] Platform::BootPerformanceInfo readBootPerformance() override
    {
        return {};
    }
};

/// A running helper that answers a CrashesRequest with another user's crash.
[[nodiscard]] std::shared_ptr<Mocks::FakeBroker> helperWithEveryUsersCrashes()
{
    auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/true);
    broker->setAnswer(
        [](const Platform::Broker::Request& request) -> std::expected<Platform::Broker::Response, std::string>
        {
            if (!std::holds_alternative<Platform::Broker::CrashesRequest>(request))
            {
                return std::unexpected(std::string("unexpected request"));
            }
            Platform::Broker::CrashesReply reply;
            reply.info.available = true;
            reply.info.listed = true;
            reply.info.journalRead = true;
            Platform::CrashEvent theirs;
            theirs.application = "postgres";
            theirs.signal = "SIGSEGV";
            reply.info.events.push_back(theirs);
            return reply;
        });
    return broker;
}

TEST(PanelPlatformsTest, TheCrashHistoryReadsEveryUsersCrashesThroughARunningHelper)
{
    const std::shared_ptr<Mocks::FakeBroker> broker = helperWithEveryUsersCrashes();
    const std::shared_ptr<Platform::ISystemInfoProbe> probe = crashHistoryProbe(std::make_unique<OwnCrashesOnlyProbe>(), broker);
    ASSERT_NE(probe, nullptr);

    // As ShellPanels::prepareAttach() wires it: Process Details' refresh reads through this probe.
    Domain::CrashHistory history([&probe] { return probe->readCrashes(); });
    history.read();

    const Platform::CrashesInfo& crashes = history.snapshot()->crashes;
    EXPECT_FALSE(crashes.otherUsersHidden) << "the unbrokered read would hide the other users' crashes again";
    ASSERT_EQ(crashes.events.size(), 1U);
    EXPECT_EQ(crashes.events.front().application, "postgres");
    EXPECT_EQ(crashes.events.front().signal, "SIGSEGV");
    EXPECT_EQ(broker->requests().size(), 1U);
}

// #2416 (#2479): the crash history the app makes reads through the shared helper, as System Information's
// probe does -- not only the seam above. An unbrokered probe would take every other user's crashes away.
TEST(PanelPlatformsTest, TheAppsCrashHistoryProbeGoesThroughTheSharedHelper)
{
    Detail::setSharedPrivilegedBrokerForTesting(helperWithEveryUsersCrashes());
    const std::shared_ptr<Platform::ISystemInfoProbe> probe = makeCrashHistoryProbe(nullptr);
    const std::unique_ptr<Platform::ISystemInfoProbe> systemInfo = makeSystemInfoPanelProbe(nullptr);
    Detail::setSharedPrivilegedBrokerForTesting(nullptr);

    ASSERT_NE(probe, nullptr);
    EXPECT_TRUE(probe->canReadAuthorized()) << "made without the helper";
    EXPECT_TRUE(systemInfo->canReadAuthorized());
}

TEST(PanelPlatformsTest, TheCrashHistoryNeverStartsTheHelper)
{
    const std::shared_ptr<Mocks::FakeBroker> broker = helperWithEveryUsersCrashes();
    broker->setConnected(false);
    const std::shared_ptr<Platform::ISystemInfoProbe> probe = crashHistoryProbe(std::make_unique<OwnCrashesOnlyProbe>(), broker);

    const Platform::CrashesInfo crashes = probe->readCrashes();
    EXPECT_TRUE(crashes.otherUsersHidden) << "no helper running: this user's own read stands";
    ASSERT_EQ(crashes.events.size(), 1U);
    EXPECT_EQ(crashes.events.front().application, "mine");
    EXPECT_EQ(broker->connectCount(), 0) << "no password prompt from a background refresh";
    EXPECT_TRUE(broker->requests().empty());
}

TEST(PanelPlatformsTest, WithoutABrokerTheCrashHistoryReadsInProcess)
{
    const std::shared_ptr<Platform::ISystemInfoProbe> probe = crashHistoryProbe(std::make_unique<OwnCrashesOnlyProbe>(), nullptr);
    ASSERT_NE(probe, nullptr);
    EXPECT_TRUE(probe->readCrashes().otherUsersHidden);
    EXPECT_FALSE(probe->canReadAuthorized());
}

} // namespace
} // namespace App
