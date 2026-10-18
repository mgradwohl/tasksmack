// Tests for BrokeredSystemInfoProbe (#2310): System Information's root-only facts are fetched through a
// running helper and parsed as the in-process read parses them; nothing is asked of a helper that isn't
// running, or for a fact the in-process read already has.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredSystemInfoProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{
namespace
{

/// An inner probe whose firmware and memory reads report what a non-root Linux read does.
class StubSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    FirmwareInfo firmware;
    MemoryModulesInfo memory;
    CrashesInfo crashes;

    [[nodiscard]] SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = true, .unavailableReason = {}, .wifiDetailsGated = false, .hasPublicIp = false};
    }
    [[nodiscard]] OsInfo readOs() override
    {
        return {};
    }
    [[nodiscard]] FirmwareInfo readFirmware() override
    {
        return firmware;
    }
    [[nodiscard]] MemoryModulesInfo readMemoryModules() override
    {
        return memory;
    }
    [[nodiscard]] CommitPagingInfo readCommitPaging() override
    {
        return {};
    }
    [[nodiscard]] StorageInfo readStorage() override
    {
        return {};
    }
    [[nodiscard]] GraphicsInfo readGraphics() override
    {
        return {};
    }
    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override
    {
        return {};
    }
    [[nodiscard]] SensorsInfo readSensors() override
    {
        return {};
    }
    [[nodiscard]] DevicesInfo readDevices() override
    {
        return {};
    }
    [[nodiscard]] DriversInfo readDrivers() override
    {
        return {};
    }
    [[nodiscard]] CrashesInfo readCrashes() override
    {
        return crashes;
    }
    [[nodiscard]] NetworkAdaptersInfo readNetworkAdapters(bool /*includeWifiDetails*/) override
    {
        return {};
    }
    [[nodiscard]] BootPerformanceInfo readBootPerformance() override
    {
        return {};
    }
};

[[nodiscard]] std::vector<std::uint8_t> bytesOf(std::string_view text)
{
    return {text.begin(), text.end()};
}

/// A 64-bit "_SM3_" entry point of SMBIOS 3.5.
[[nodiscard]] std::vector<std::uint8_t> entryPoint35()
{
    std::vector<std::uint8_t> bytes(0x18, 0);
    const std::string_view anchor = "_SM3_";
    std::ranges::copy(anchor, bytes.begin());
    bytes.at(7) = 3;
    bytes.at(8) = 5;
    return bytes;
}

/// A table of one populated memory device: SMBIOS 2.1's type 17 (21 bytes), 8 GiB DDR5 in "DIMM 1".
[[nodiscard]] std::vector<std::uint8_t> oneModuleTable()
{
    std::vector<std::uint8_t> bytes(0x15, 0);
    bytes.at(0) = 17;   // memory device
    bytes.at(1) = 0x15; // length
    bytes.at(2) = 0x00; // handle 0x1100
    bytes.at(3) = 0x11;
    bytes.at(0x0C) = 0x00; // 0x2000 MB
    bytes.at(0x0D) = 0x20;
    bytes.at(0x0E) = 0x09; // DIMM
    bytes.at(0x10) = 1;    // locator: string 1
    bytes.at(0x11) = 2;    // bank locator: string 2
    bytes.at(0x12) = 0x22; // DDR5
    const std::string_view strings{"DIMM 1\0BANK 0\0\0", 15};
    bytes.insert(bytes.end(), strings.begin(), strings.end());
    return bytes;
}

struct Fixture
{
    explicit Fixture(bool connected) : broker(std::make_shared<Mocks::FakeBroker>(connected))
    {
        auto owned = std::make_unique<StubSystemInfoProbe>();
        inner = owned.get();
        inner->firmware.available = true;
        inner->firmware.identifiersNeedAdmin = true;
        inner->firmware.smbiosVersionNeedsAdmin = true;
        inner->memory.available = true;
        inner->memory.tableNeedsAdmin = true;
        inner->memory.usableBytes = 8;
        probe = std::make_unique<BrokeredSystemInfoProbe>(std::move(owned), broker);
        broker->setAnswer(
            [](const Broker::Request& request) -> std::expected<Broker::Response, std::string>
            {
                const auto* read = std::get_if<Broker::FirmwareFileRequest>(&request);
                if (read == nullptr)
                {
                    return std::unexpected(std::string("unexpected request"));
                }
                switch (read->file)
                {
                case Broker::FirmwareFile::ProductSerial:
                    return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = bytesOf("PF1ABCDE\n")};
                case Broker::FirmwareFile::ProductUuid:
                    return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok,
                                                     .bytes = bytesOf("4c4c4544-0042-3510-8051-b4c04f4a4b33\n")};
                case Broker::FirmwareFile::BoardSerial:
                    return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::NotFound, .bytes = {}};
                case Broker::FirmwareFile::SmbiosEntryPoint:
                    return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = entryPoint35()};
                case Broker::FirmwareFile::SmbiosTable:
                    return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = oneModuleTable()};
                }
                return std::unexpected(std::string("unknown file"));
            });
    }

    std::shared_ptr<Mocks::FakeBroker> broker;
    StubSystemInfoProbe* inner = nullptr;
    std::unique_ptr<BrokeredSystemInfoProbe> probe;
};

TEST(BrokeredSystemInfoProbeTest, WithoutARunningHelperTheRootOnlyFactsStayMarked)
{
    Fixture f(/*connected=*/false);
    const FirmwareInfo firmware = f.probe->readFirmware();
    EXPECT_TRUE(firmware.identifiersNeedAdmin);
    EXPECT_TRUE(firmware.smbiosVersionNeedsAdmin);
    EXPECT_TRUE(f.probe->readMemoryModules().tableNeedsAdmin);
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredSystemInfoProbeTest, ARunningHelperFillsTheIdentifiersAndSmbiosVersion)
{
    Fixture f(/*connected=*/true);
    const FirmwareInfo firmware = f.probe->readFirmware();
    EXPECT_FALSE(firmware.identifiersNeedAdmin);
    EXPECT_EQ(firmware.systemSerial, "PF1ABCDE");
    EXPECT_EQ(firmware.systemUuid, "4c4c4544-0042-3510-8051-b4c04f4a4b33");
    EXPECT_EQ(firmware.boardSerial, ""); // the machine has none: not reported, no longer "needs admin"
    EXPECT_FALSE(firmware.smbiosVersionNeedsAdmin);
    EXPECT_EQ(firmware.smbiosVersion, "3.5");
}

TEST(BrokeredSystemInfoProbeTest, ARunningHelperReadsTheMemoryTable)
{
    Fixture f(/*connected=*/true);
    const MemoryModulesInfo memory = f.probe->readMemoryModules();
    EXPECT_FALSE(memory.tableNeedsAdmin);
    EXPECT_TRUE(memory.tableRead);
    ASSERT_EQ(memory.modules.size(), 1U);
    EXPECT_EQ(memory.modules.front().locator, "DIMM 1");
    EXPECT_EQ(memory.modules.front().sizeBytes, std::uint64_t{8} << 30U);
    EXPECT_EQ(memory.usableBytes, 8U); // the inner read's facts are kept
    EXPECT_EQ(f.broker->requests(), (std::vector<Broker::Request>{Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable}}));
}

TEST(BrokeredSystemInfoProbeTest, AFileTheHelperCantReadStaysMarked)
{
    Fixture f(/*connected=*/true);
    f.broker->setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string>
                        { return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}}; });
    const FirmwareInfo firmware = f.probe->readFirmware();
    EXPECT_TRUE(firmware.identifiersNeedAdmin);
    EXPECT_TRUE(firmware.smbiosVersionNeedsAdmin);
    EXPECT_TRUE(f.probe->readMemoryModules().tableNeedsAdmin);
}

TEST(BrokeredSystemInfoProbeTest, FactsTheInProcessReadHasAreNotFetched)
{
    Fixture f(/*connected=*/true);
    f.inner->firmware = FirmwareInfo{};
    f.inner->firmware.available = true;
    f.inner->firmware.systemSerial = "MINE";
    f.inner->memory = MemoryModulesInfo{};
    f.inner->memory.available = true;
    f.inner->memory.tableRead = true;
    EXPECT_EQ(f.probe->readFirmware().systemSerial, "MINE");
    static_cast<void>(f.probe->readMemoryModules());
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredSystemInfoProbeTest, EveryUsersCrashesComeFromARunningHelper)
{
    Fixture f(/*connected=*/true);
    f.inner->crashes.available = true;
    f.inner->crashes.journalRead = true;
    f.inner->crashes.otherUsersHidden = true;
    f.broker->setAnswer(
        [](const Broker::Request& request) -> std::expected<Broker::Response, std::string>
        {
            if (!std::holds_alternative<Broker::CrashesRequest>(request))
            {
                return std::unexpected(std::string("unexpected request"));
            }
            Broker::CrashesReply reply;
            reply.info.available = true;
            reply.info.listed = true;
            CrashEvent event;
            event.application = "postgres";
            reply.info.events.push_back(event);
            return reply;
        });
    const CrashesInfo crashes = f.probe->readCrashes();
    EXPECT_FALSE(crashes.otherUsersHidden);
    ASSERT_EQ(crashes.events.size(), 1U);
    EXPECT_EQ(crashes.events.front().application, "postgres");
}

TEST(BrokeredSystemInfoProbeTest, CrashesAreTheInnerReadsWhenNothingIsHidden)
{
    Fixture f(/*connected=*/true);
    f.inner->crashes.available = true;
    f.inner->crashes.listed = true;
    EXPECT_TRUE(f.probe->readCrashes().listed);
    EXPECT_TRUE(f.broker->requests().empty());

    f.inner->crashes.otherUsersHidden = true;
    f.broker->setConnected(false);
    EXPECT_TRUE(f.probe->readCrashes().otherUsersHidden) << "no helper running: the hidden ones stay hidden";
}

TEST(BrokeredSystemInfoProbeTest, AuthorizeStartsTheHelper)
{
    Fixture f(/*connected=*/false);
    EXPECT_TRUE(f.probe->canReadAuthorized());
    EXPECT_TRUE(f.probe->authorize(std::stop_token{}));
    EXPECT_EQ(f.broker->connectCount(), 1);
    EXPECT_EQ(f.probe->readFirmware().systemSerial, "PF1ABCDE");

    EXPECT_TRUE(f.probe->authorize(std::stop_token{})); // already running: no second prompt
    EXPECT_EQ(f.broker->connectCount(), 1);
}

TEST(BrokeredSystemInfoProbeTest, ADismissedPromptAuthorizesNothing)
{
    Fixture f(/*connected=*/false);
    f.broker->setConnectFailure("the password prompt was dismissed");
    EXPECT_FALSE(f.probe->authorize(std::stop_token{}));
    EXPECT_TRUE(f.probe->readFirmware().identifiersNeedAdmin);
}

TEST(BrokeredSystemInfoProbeTest, WithoutABrokerTheProbeIsUndecorated)
{
    const std::unique_ptr<ISystemInfoProbe> probe = withPrivilegedBroker(std::make_unique<StubSystemInfoProbe>(), nullptr);
    EXPECT_FALSE(probe->canReadAuthorized());
    EXPECT_FALSE(probe->authorize(std::stop_token{}));
}

} // namespace
} // namespace Platform
