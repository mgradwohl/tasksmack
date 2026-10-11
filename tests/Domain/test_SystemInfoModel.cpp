/// @file test_SystemInfoModel.cpp
/// @brief Domain::SystemInfoModel (#1399): nothing is read until read() is called, each read publishes
/// a new immutable snapshot, and a probe without OS facts is never asked for them.

#include "Domain/CrashHistory.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace Domain
{
namespace
{

/// How FakeSystemInfoProbe::readOs() fails, if at all.
enum class ReadOsThrows : std::uint8_t
{
    No,
    StdException, ///< std::runtime_error("no WMI")
    NonStandard,  ///< an int
};

class FakeSystemInfoProbe final : public Platform::ISystemInfoProbe
{
  public:
    FakeSystemInfoProbe(bool hasOs,
                        int* reads,
                        ReadOsThrows throws = ReadOsThrows::No,
                        std::chrono::milliseconds readOsDelay = std::chrono::milliseconds(0))
        : m_HasOs(hasOs), m_Reads(reads), m_Throws(throws), m_ReadOsDelay(readOsDelay)
    {}

    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = m_HasOs, .unavailableReason = m_HasOs ? std::string{} : std::string{"not here"}};
    }

    [[nodiscard]] Platform::OsInfo readOs() override
    {
        ++*m_Reads;
        std::this_thread::sleep_for(m_ReadOsDelay);
        if (m_Throws == ReadOsThrows::StdException)
        {
            throw std::runtime_error("no WMI");
        }
        if (m_Throws == ReadOsThrows::NonStandard)
        {
            // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass) - intentionally not a std::exception
            throw 42;
        }
        Platform::OsInfo info;
        info.family = Platform::OsFamily::Windows;
        info.name = "Windows 11 Pro";
        info.build = "26100." + std::to_string(*m_Reads);
        return info;
    }

    [[nodiscard]] Platform::FirmwareInfo readFirmware() override
    {
        Platform::FirmwareInfo info;
        info.available = true;
        info.systemManufacturer = "Contoso";
        return info;
    }

    [[nodiscard]] Platform::MemoryModulesInfo readMemoryModules() override
    {
        Platform::MemoryModulesInfo info;
        info.available = true;
        info.slotCount = 2;
        return info;
    }

    [[nodiscard]] Platform::CommitPagingInfo readCommitPaging() override
    {
        Platform::CommitPagingInfo info;
        info.available = true;
        info.committedBytes = 4096;
        return info;
    }

    [[nodiscard]] Platform::StorageInfo readStorage() override
    {
        Platform::StorageInfo info;
        info.available = true;
        Platform::PhysicalDisk disk;
        disk.name = "Disk 0";
        info.disks.push_back(disk);
        return info;
    }

    [[nodiscard]] Platform::GraphicsInfo readGraphics() override
    {
        Platform::GraphicsInfo info;
        info.available = true;
        Platform::GraphicsAdapter adapter;
        adapter.name = "Contoso GPU";
        info.adapters.push_back(adapter);
        return info;
    }

    [[nodiscard]] Platform::PlatformSecurityInfo readPlatformSecurity() override
    {
        Platform::PlatformSecurityInfo info;
        info.available = true;
        info.secureBoot = Platform::SecurityFeatureState::On;
        return info;
    }

    [[nodiscard]] Platform::SensorsInfo readSensors() override
    {
        Platform::SensorsInfo info;
        info.available = true;
        info.listed = true;
        // One temperature that reads 50, 40, 60 °C, then repeats: a range to track (#1522).
        constexpr std::array<double, 3> TEMPERATURES{50.0, 40.0, 60.0};
        Platform::SensorDevice device;
        device.name = "coretemp";
        device.readings.push_back({.kind = Platform::SensorKind::Temperature,
                                   .label = "Package id 0",
                                   .value = TEMPERATURES.at(m_SensorReads++ % TEMPERATURES.size()),
                                   .high = std::nullopt,
                                   .critical = std::nullopt});
        info.devices.push_back(std::move(device));
        return info;
    }

    [[nodiscard]] Platform::DevicesInfo readDevices() override
    {
        Platform::DevicesInfo info;
        info.available = true;
        info.pciRead = true;
        return info;
    }

    [[nodiscard]] Platform::DriversInfo readDrivers() override
    {
        Platform::DriversInfo info;
        info.available = true;
        info.listed = true;
        return info;
    }

    [[nodiscard]] Platform::CrashesInfo readCrashes() override
    {
        Platform::CrashesInfo info;
        info.available = true;
        info.listed = true;
        return info;
    }

    [[nodiscard]] Platform::NetworkAdaptersInfo readNetworkAdapters() override
    {
        Platform::NetworkAdaptersInfo info;
        info.available = true;
        info.gatewayV4 = "192.168.1.1";
        return info;
    }

    [[nodiscard]] Platform::BootPerformanceInfo readBootPerformance() override
    {
        Platform::BootPerformanceInfo info;
        info.available = true;
        info.timingsRead = true;
        info.totalUs = 12'000'000;
        return info;
    }

  private:
    bool m_HasOs;
    int* m_Reads;
    ReadOsThrows m_Throws;
    std::chrono::milliseconds m_ReadOsDelay;
    std::size_t m_SensorReads = 0;
};

TEST(SystemInfoModelTest, ReadsOnlyWhenAskedAndPublishesEachRead)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    EXPECT_TRUE(model.capabilities().hasOs);
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(model.version(), 0U);
    EXPECT_EQ(model.snapshot()->os.family, Platform::OsFamily::Unknown);

    model.read();
    const auto first = model.snapshot();
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(first->version, 1U);
    EXPECT_EQ(first->os.name, "Windows 11 Pro");
    EXPECT_EQ(first->os.build, "26100.1");
    EXPECT_EQ(first->firmware.systemManufacturer, "Contoso");
    EXPECT_EQ(first->memory.slotCount, 2U);
    EXPECT_EQ(first->paging.committedBytes, 4096U);
    ASSERT_EQ(first->storage.disks.size(), 1U);
    EXPECT_EQ(first->storage.disks[0].name, "Disk 0");
    ASSERT_EQ(first->graphics.adapters.size(), 1U);
    EXPECT_EQ(first->graphics.adapters[0].name, "Contoso GPU");
    EXPECT_EQ(first->security.secureBoot, Platform::SecurityFeatureState::On);
    EXPECT_TRUE(first->sensors.listed);
    EXPECT_TRUE(first->devices.pciRead);
    EXPECT_TRUE(first->drivers.listed);
    EXPECT_TRUE(first->crashes.listed);
    EXPECT_EQ(first->adapters.gatewayV4, "192.168.1.1");
    EXPECT_EQ(first->boot.totalUs, std::optional<std::uint64_t>(12'000'000));
    EXPECT_GT(first->readAtUnixSeconds, 0U);

    model.read(); // Refresh
    EXPECT_EQ(model.version(), 2U);
    EXPECT_EQ(model.snapshot()->os.build, "26100.2");
    EXPECT_EQ(first->os.build, "26100.1"); // the earlier snapshot is unchanged
}

// #1522: the sensors read again on their own, with every other fact (and when they were read) kept, and
// each sensor's range over the reads.
TEST(SystemInfoModelTest, ReadSensorsKeepsTheRestAndTracksEachSensorsRange)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    model.readSensors(); // before the first read: nothing to keep the rest of
    EXPECT_EQ(model.version(), 0U);

    model.read();
    const auto first = model.snapshot();
    const std::string key = sensorRangeKey("coretemp", "Package id 0");
    ASSERT_TRUE(first->sensorRanges.contains(key));
    EXPECT_EQ(first->sensorRanges.at(key), (SensorRange{.min = 50.0, .max = 50.0, .samples = 1}));

    model.readSensors();
    model.readSensors();
    const auto latest = model.snapshot();
    EXPECT_EQ(latest->version, 3U);
    EXPECT_EQ(reads, 1);                          // the OS wasn't read again
    EXPECT_EQ(latest->os.build, first->os.build); // nor anything else
    EXPECT_EQ(latest->readAtUnixSeconds, first->readAtUnixSeconds);
    ASSERT_EQ(latest->sensors.devices.size(), 1U);
    EXPECT_DOUBLE_EQ(latest->sensors.devices[0].readings[0].value, 60.0);
    EXPECT_EQ(latest->sensorRanges.at(key), (SensorRange{.min = 40.0, .max = 60.0, .samples = 3}));
    EXPECT_EQ(first->sensorRanges.at(key).samples, 1U); // an earlier snapshot is unchanged

    // A full read (Refresh) adds to the ranges rather than starting them again.
    model.read();
    EXPECT_EQ(model.snapshot()->sensorRanges.at(key).samples, 4U);
    EXPECT_FALSE(model.tryReadSensors().has_value());
}

// #1773: a read is stamped when it starts, so the crash cache it publishes to orders it correctly against
// a read that started later and finished first.
TEST(SystemInfoModelTest, AReadIsStampedWhenItStarts)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads, ReadOsThrows::No, std::chrono::milliseconds(2100)));
    const auto before = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    model.read();
    EXPECT_LE(model.snapshot()->readAtUnixSeconds, before + 1); // finished two seconds later
    EXPECT_GE(model.snapshot()->readAtUnixSeconds, before);
}

TEST(SystemInfoModelTest, UnsupportedProbeIsNotAskedForFacts)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(false, &reads));
    EXPECT_EQ(model.capabilities().unavailableReason, "not here");
    model.read();
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(model.snapshot()->os.family, Platform::OsFamily::Unknown);
    EXPECT_FALSE(model.snapshot()->firmware.available);
    EXPECT_FALSE(model.snapshot()->memory.available);
    EXPECT_FALSE(model.snapshot()->paging.available);
    EXPECT_FALSE(model.snapshot()->storage.available);
    EXPECT_FALSE(model.snapshot()->graphics.available);
    EXPECT_FALSE(model.snapshot()->security.available);
    EXPECT_FALSE(model.snapshot()->sensors.available);
    EXPECT_FALSE(model.snapshot()->devices.available);
    EXPECT_FALSE(model.snapshot()->drivers.available);
    EXPECT_FALSE(model.snapshot()->crashes.available);
    EXPECT_FALSE(model.snapshot()->adapters.available);
    EXPECT_FALSE(model.snapshot()->boot.available);

    const SystemInfoModel stub(std::make_unique<Platform::UnsupportedSystemInfoProbe>());
    EXPECT_FALSE(stub.capabilities().hasOs);
}

TEST(SystemInfoModelTest, HandsEachReadsCrashesToTheSharedCrashHistory)
{
    // Process Details' crash line (#1675) shows the System tab's read without reading the log itself.
    int historyReads = 0;
    const auto history = std::make_shared<CrashHistory>(
        [&historyReads]
        {
            ++historyReads;
            return Platform::CrashesInfo{};
        });
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads), history);
    model.read();
    EXPECT_EQ(history->version(), 1U);
    EXPECT_TRUE(history->snapshot()->crashes.listed);
    EXPECT_EQ(history->snapshot()->readAtUnixSeconds, model.snapshot()->readAtUnixSeconds);
    EXPECT_EQ(historyReads, 0);

    // A probe without OS facts publishes nothing: the history's own read stays due.
    const auto untouched = std::make_shared<CrashHistory>([] { return Platform::CrashesInfo{}; });
    SystemInfoModel unsupported(std::make_unique<FakeSystemInfoProbe>(false, &reads), untouched);
    unsupported.read();
    EXPECT_EQ(untouched->version(), 0U);
}

TEST(SystemInfoModelTest, ReadThatThrowsBecomesAPlainFailureMessage)
{
    // tryRead() runs on the System tab's worker: a failed read comes back as a copied message, never
    // as an exception through the future (#1706). Nothing is published for it.
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads, ReadOsThrows::StdException));
    EXPECT_EQ(model.tryRead(), std::optional<std::string>("no WMI"));
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(model.version(), 0U);

    SystemInfoModel nonStandard(std::make_unique<FakeSystemInfoProbe>(true, &reads, ReadOsThrows::NonStandard));
    EXPECT_EQ(nonStandard.tryRead(), std::optional<std::string>("unknown error"));
    EXPECT_EQ(nonStandard.version(), 0U);

    SystemInfoModel fine(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    EXPECT_EQ(fine.tryRead(), std::nullopt);
    EXPECT_EQ(fine.version(), 1U);
}

TEST(SystemInfoModelTest, RequiresAProbe)
{
    EXPECT_THROW(SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe>{}), std::invalid_argument);
}

} // namespace
} // namespace Domain
