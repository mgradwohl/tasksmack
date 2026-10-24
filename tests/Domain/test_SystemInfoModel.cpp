/// @file test_SystemInfoModel.cpp
/// @brief Domain::SystemInfoModel (#1399): nothing is read until read() is called, each read publishes
/// a new immutable snapshot, a probe without OS facts is never asked for them, and each sensor's range
/// and bounded history (#1522).

#include "Domain/CrashHistory.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SensorTracker.h"
#include "Domain/SystemInfoModel.h"
#include "Mocks/UnsupportedSystemInfoProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
        return {.hasOs = m_HasOs,
                .unavailableReason = m_HasOs ? std::string{} : std::string{"not here"},
                .wifiDetailsGated = false,
                .hasPublicIp = m_PublicIpReads != nullptr,
                .canReadAuthorized = canReadAuthorized()};
    }

    /// Offers authorize() (#2310), counting its calls in @p authorizations.
    void offerAuthorization(int* authorizations) noexcept
    {
        m_Authorizations = authorizations;
    }

    /// The helper authorize() starts turns out to read only the user's own processes (#2769): from
    /// then on this can't read as an administrator, as BrokeredSystemInfoProbe's answer changes.
    void takeAuthorizationBackOnAuthorize() noexcept
    {
        m_TakeBackOnAuthorize = true;
    }

    [[nodiscard]] bool canReadAuthorized() const override
    {
        return m_Authorizations != nullptr && !m_AuthorizationTakenBack;
    }

    bool authorize(const std::stop_token& stopToken) override
    {
        m_AuthorizeSawStop = stopToken.stop_requested();
        if (m_Authorizations != nullptr)
        {
            ++*m_Authorizations;
        }
        m_AuthorizationTakenBack = m_TakeBackOnAuthorize;
        return m_Authorizations != nullptr;
    }

    /// Whether the last authorize() was handed a stopped token (#2381).
    [[nodiscard]] bool authorizeSawStop() const noexcept
    {
        return m_AuthorizeSawStop;
    }

    /// Reports a public IP lookup (#1518), counting its calls in @p reads.
    void lookUpPublicIp(int* reads) noexcept
    {
        m_PublicIpReads = reads;
    }

    [[nodiscard]] Platform::PublicIpInfo readPublicIp() override
    {
        if (m_PublicIpReads != nullptr)
        {
            ++*m_PublicIpReads;
        }
        return {.read = true,
                .v4 = "203.0.113.7",
                .v6 = {},
                .v4Unavailable = {},
                .v6Unavailable = "No IPv6 route to the internet",
                .source = "OpenDNS"};
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

    /// Each sensors read returns the next of @p reads (the last one again once they run out), in place
    /// of the one coretemp sensor (#2340).
    void scriptSensors(std::vector<Platform::SensorsInfo> reads)
    {
        m_ScriptedSensors = std::move(reads);
    }

    [[nodiscard]] Platform::SensorsInfo readSensors() override
    {
        if (!m_ScriptedSensors.empty())
        {
            return m_ScriptedSensors.at(std::min(m_SensorReads++, m_ScriptedSensors.size() - 1));
        }
        Platform::SensorsInfo info;
        info.available = true;
        info.listed = true;
        // One temperature that reads 50, 40, 60 °C, then repeats: a range to track (#1522).
        constexpr std::array<double, 3> TEMPERATURES{50.0, 40.0, 60.0};
        Platform::SensorDevice device;
        device.name = "coretemp";
        device.id = "hwmon2";
        device.readings.push_back({.kind = Platform::SensorKind::Temperature,
                                   .label = "Package id 0",
                                   .id = "temp1",
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

    [[nodiscard]] Platform::NetworkAdaptersInfo readNetworkAdapters(bool includeWifiDetails) override
    {
        Platform::NetworkAdaptersInfo info;
        info.available = true;
        info.gatewayV4 = "192.168.1.1";
        // One Wi-Fi adapter whose details are withheld unless asked for, as Windows' are (#1782).
        Platform::NetworkAdapter wifi;
        wifi.name = "Wi-Fi";
        wifi.wireless = true;
        wifi.wifiDetailsWithheld = !includeWifiDetails;
        info.adapters.push_back(std::move(wifi));
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
    std::vector<Platform::SensorsInfo> m_ScriptedSensors;
    int* m_PublicIpReads = nullptr;
    int* m_Authorizations = nullptr;
    bool m_AuthorizeSawStop = false;
    bool m_TakeBackOnAuthorize = false;
    bool m_AuthorizationTakenBack = false;
};

/// The key of the coretemp sensor FakeSystemInfoProbe reads by default.
[[nodiscard]] std::string coretempKey()
{
    return sensorKey(Platform::SensorDevice{.name = "coretemp", .id = "hwmon2", .readings = {}},
                     Platform::SensorReading{.kind = Platform::SensorKind::Temperature,
                                             .label = "Package id 0",
                                             .id = "temp1",
                                             .value = 0.0,
                                             .high = std::nullopt,
                                             .critical = std::nullopt});
}

/// The sensor sampler's model (#2737) over @p probe's sensors, on a clock that moves 2 s on at each
/// read: one sample() stands for one read of the sensors.
[[nodiscard]] std::unique_ptr<SensorModel> sensorModelOver(std::unique_ptr<FakeSystemInfoProbe> probe)
{
    const std::shared_ptr<FakeSystemInfoProbe> shared = std::move(probe);
    auto now = std::make_shared<SensorModel::Clock::time_point>(std::chrono::hours(1));
    return std::make_unique<SensorModel>([shared] { return SensorModel::Reader([shared] { return shared->readSensors(); }); },
                                         std::chrono::milliseconds(Sampling::SYSTEM_INFO_SENSOR_REFRESH_MS),
                                         [now]
                                         {
                                             *now += std::chrono::seconds(2);
                                             return *now;
                                         });
}

/// A temperature or other reading for a scripted sensors read (#2340).
[[nodiscard]] Platform::SensorReading scriptedReading(Platform::SensorKind kind, std::string label, std::string id, double value)
{
    return {.kind = kind, .label = std::move(label), .id = std::move(id), .value = value, .high = std::nullopt, .critical = std::nullopt};
}

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

// #1782: the Wi-Fi details that can raise a location prompt are left out of every read until asked for,
// then included in every read after, Refresh too; a sensors-only read keeps what the last full read had.
TEST(SystemInfoModelTest, WifiDetailsOnlyOnceAskedThenForTheSession)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    EXPECT_FALSE(model.wifiDetailsIncluded());
    model.read();
    ASSERT_EQ(model.snapshot()->adapters.adapters.size(), 1U);
    EXPECT_TRUE(model.snapshot()->adapters.adapters[0].wifiDetailsWithheld) << "the first read doesn't ask";
    EXPECT_FALSE(model.snapshot()->wifiDetailsIncluded);

    model.includeWifiDetails();
    EXPECT_TRUE(model.wifiDetailsIncluded());
    model.read();
    EXPECT_FALSE(model.snapshot()->adapters.adapters[0].wifiDetailsWithheld);
    EXPECT_TRUE(model.snapshot()->wifiDetailsIncluded);

    model.read(); // Refresh
    EXPECT_FALSE(model.snapshot()->adapters.adapters[0].wifiDetailsWithheld) << "the choice holds";
    EXPECT_TRUE(model.snapshot()->wifiDetailsIncluded);
}

// #1518: the public addresses are looked up (a query off the machine) only once asked for, then on every
// read after, Refresh too.
TEST(SystemInfoModelTest, PublicIpOnlyOnceAskedThenForTheSession)
{
    int reads = 0;
    int lookups = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->lookUpPublicIp(&lookups);
    SystemInfoModel model(std::move(probe));
    EXPECT_TRUE(model.capabilities().hasPublicIp);
    EXPECT_FALSE(model.publicIpIncluded());
    model.read();
    EXPECT_EQ(lookups, 0) << "the first read doesn't ask";
    EXPECT_FALSE(model.snapshot()->publicIpIncluded);
    EXPECT_FALSE(model.snapshot()->publicIp.read);

    model.includePublicIp();
    EXPECT_TRUE(model.publicIpIncluded());
    model.read();
    EXPECT_EQ(lookups, 1);
    EXPECT_TRUE(model.snapshot()->publicIpIncluded);
    EXPECT_EQ(model.snapshot()->publicIp.v4, "203.0.113.7");
    EXPECT_EQ(model.snapshot()->publicIp.v6Unavailable, "No IPv6 route to the internet");

    model.read(); // Refresh
    EXPECT_EQ(lookups, 2) << "the choice holds";
}

// #2310: "Authenticate to read" asks for the password once, before the next read, not on every read.
TEST(SystemInfoModelTest, AuthorizationIsAskedOnceBeforeTheNextRead)
{
    int reads = 0;
    int authorizations = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->offerAuthorization(&authorizations);
    SystemInfoModel model(std::move(probe));
    EXPECT_TRUE(model.capabilities().canReadAuthorized);
    model.read();
    EXPECT_EQ(authorizations, 0) << "never without a click";

    model.requestAuthorization();
    model.read();
    EXPECT_EQ(authorizations, 1);
    EXPECT_EQ(reads, 2) << "the read goes on after the prompt";
    model.read();
    EXPECT_EQ(authorizations, 1) << "the helper stays authorized by itself";
}

// #2381: the read's stop token reaches the prompt, so closing the page doesn't wait the prompt out.
TEST(SystemInfoModelTest, TheReadsStopTokenReachesAuthorize)
{
    int reads = 0;
    int authorizations = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->offerAuthorization(&authorizations);
    const FakeSystemInfoProbe* fake = probe.get();
    SystemInfoModel model(std::move(probe));
    std::stop_source stop;
    stop.request_stop();
    model.requestAuthorization();
    EXPECT_FALSE(model.tryRead(stop.get_token()).has_value());
    EXPECT_EQ(authorizations, 1);
    EXPECT_TRUE(fake->authorizeSawStop());
}

// #2769: a helper that turns out to read only the user's own processes takes "Authenticate to read"
// back: the page stops offering it from that read on, and no later request asks the prompt again.
TEST(SystemInfoModelTest, AuthorizationTheHelperTakesBackIsntOfferedOrAskedAgain)
{
    int reads = 0;
    int authorizations = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->offerAuthorization(&authorizations);
    probe->takeAuthorizationBackOnAuthorize();
    SystemInfoModel model(std::move(probe));
    EXPECT_TRUE(model.capabilities().canReadAuthorized);
    model.read();
    EXPECT_TRUE(model.capabilities().canReadAuthorized) << "a read without the prompt teaches nothing";

    model.requestAuthorization();
    model.read();
    EXPECT_EQ(authorizations, 1);
    EXPECT_EQ(reads, 2) << "the read goes on after the prompt";
    EXPECT_FALSE(model.capabilities().canReadAuthorized) << "taken back by the read that learned it";

    model.requestAuthorization();
    model.read();
    EXPECT_EQ(authorizations, 1) << "never asked again";
    EXPECT_EQ(reads, 3);
    EXPECT_FALSE(model.capabilities().canReadAuthorized);
}

TEST(SystemInfoModelTest, AuthorizationIsNeverAskedOfAProbeWithoutIt)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    EXPECT_FALSE(model.capabilities().canReadAuthorized);
    model.requestAuthorization();
    model.read();
    EXPECT_EQ(reads, 1);
}

// #1518: a probe without a lookup is never asked, even once the user asked.
TEST(SystemInfoModelTest, PublicIpNeverAskedOfAProbeWithoutALookup)
{
    int reads = 0;
    SystemInfoModel model(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    EXPECT_FALSE(model.capabilities().hasPublicIp);
    model.includePublicIp();
    model.read();
    EXPECT_FALSE(model.snapshot()->publicIpIncluded);
    EXPECT_FALSE(model.snapshot()->publicIp.read);
}

// #1522: the sensors read again on their own, with every other fact (and when they were read) kept, and
// each sensor's range over the reads.
// #2737: the sensors are read by their own sampler, which tracks each one's range; the page's full read
// keeps no ranges of its own.
TEST(SystemInfoModelTest, TheSensorSamplerTracksEachSensorsRange)
{
    int reads = 0;
    auto sensors = sensorModelOver(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    const std::string key = coretempKey();
    sensors->sample();
    const auto first = sensors->publication();
    ASSERT_TRUE(first->ranges.contains(key));
    EXPECT_EQ(first->ranges.at(key), (SensorRange{.min = 50.0, .max = 50.0, .samples = 1}));

    sensors->sample();
    sensors->sample();
    const auto latest = sensors->publication();
    EXPECT_EQ(latest->version, 3U);
    EXPECT_EQ(reads, 0) << "nothing but the sensors was read";
    ASSERT_EQ(latest->sensors.devices.size(), 1U);
    EXPECT_DOUBLE_EQ(latest->sensors.devices[0].readings[0].value, 60.0);
    EXPECT_EQ(latest->ranges.at(key), (SensorRange{.min = 40.0, .max = 60.0, .samples = 3}));
    EXPECT_EQ(first->ranges.at(key).samples, 1U); // an earlier publication is unchanged
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

    const SystemInfoModel stub(std::make_unique<Mocks::UnsupportedSystemInfoProbe>());
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

    // A later read replaces it even when the shown one is stamped in the future (a backward clock
    // step, #2157): the ticket it took as it started orders it, not its wall-clock stamp.
    history->publish(Platform::CrashesInfo{}, 4'000'000'000);
    model.read();
    EXPECT_EQ(history->version(), 3U);
    EXPECT_EQ(history->snapshot()->readAtUnixSeconds, model.snapshot()->readAtUnixSeconds);

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

// #1522: each sensor's history for its row's chart, appended at each read, oldest first.
TEST(SystemInfoModelTest, EachSensorReadAppendsToItsHistory)
{
    int reads = 0;
    auto sensors = sensorModelOver(std::make_unique<FakeSystemInfoProbe>(true, &reads));
    const std::string key = coretempKey();
    sensors->sample(); // 50 °C
    ASSERT_TRUE(sensors->publication()->histories.contains(key));
    EXPECT_EQ(sensors->publication()->histories.at(key).values.size(), 1U);

    sensors->sample(); // 40 °C
    sensors->sample(); // 60 °C
    const SensorHistory& range = sensors->publication()->histories.at(key);
    ASSERT_EQ(range.values.size(), 3U);
    ASSERT_EQ(range.times.size(), 3U);
    EXPECT_FLOAT_EQ(range.values[0], 50.0F); // oldest first
    EXPECT_FLOAT_EQ(range.values[1], 40.0F);
    EXPECT_FLOAT_EQ(range.values[2], 60.0F);
    EXPECT_LT(range.times[0], range.times[1]);
    EXPECT_LT(range.times[1], range.times[2]);
}

// #2495: the kernel reuses a freed hwmon number for the next device, which starts a range and history of its
// own; a chip that only gains or loses its " #2" keeps its.
TEST(SystemInfoModelTest, ADeviceTakingAnUnpluggedOnesNumberStartsItsOwnRange)
{
    using Platform::SensorKind;
    const auto chip = [](const char* name, double value)
    {
        Platform::SensorsInfo info;
        info.available = true;
        info.listed = true;
        info.devices.push_back(
            {.name = name, .id = "hwmon5", .readings = {scriptedReading(SensorKind::Temperature, "temp1", "temp1_input", value)}});
        return info;
    };
    int reads = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->scriptSensors({chip("nvme", 40.0), chip("nvme", 55.0), chip("nvme #2", 50.0), chip("mt7921_phy0", 30.0)});
    auto sensors = sensorModelOver(std::move(probe));
    sensors->sample();
    sensors->sample();
    sensors->sample();
    const std::string key = std::string("hwmon5") + '\x1F' + "temp1_input";
    const auto renamed = sensors->publication();
    ASSERT_TRUE(renamed->ranges.contains(key));
    EXPECT_EQ(renamed->ranges.at(key), (SensorRange{.min = 40.0, .max = 55.0, .samples = 3})) << "only its #2 changed";

    sensors->sample();
    const auto replaced = sensors->publication();
    EXPECT_EQ(replaced->ranges.at(key), (SensorRange{.min = 30.0, .max = 30.0, .samples = 1}));
    ASSERT_TRUE(replaced->histories.contains(key));
    EXPECT_EQ(replaced->histories.at(key).values.size(), 1U);
}

// #2340: two sensors that share a label (two "acpitz" thermal zones; a voltage and a current both
// "VDD_IN") keep their own range and history, through a read that reorders the channels and one that
// drops one of them.
TEST(SystemInfoModelTest, SensorsSharingALabelKeepTheirOwnRangeAndHistory)
{
    using Platform::SensorKind;
    const auto sensors = [](std::vector<Platform::SensorReading> zones, std::vector<Platform::SensorReading> chip)
    {
        Platform::SensorsInfo info;
        info.available = true;
        info.listed = true;
        info.devices.push_back({.name = "fixture-chip", .id = "hwmon0", .readings = std::move(chip)});
        info.devices.push_back({.name = "Thermal zones", .id = "thermal", .readings = std::move(zones)});
        return info;
    };
    const auto zone0 = [](double value)
    {
        return scriptedReading(SensorKind::Temperature, "acpitz", "thermal_zone0", value);
    };
    const auto zone1 = [](double value)
    {
        return scriptedReading(SensorKind::Temperature, "acpitz", "thermal_zone1", value);
    };
    const auto volts = [](double value)
    {
        return scriptedReading(SensorKind::Voltage, "VDD_IN", "in1", value);
    };
    const auto amps = [](double value)
    {
        return scriptedReading(SensorKind::Current, "VDD_IN", "curr1", value);
    };

    int reads = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->scriptSensors({
        sensors({zone0(40.0), zone1(70.0)}, {volts(12.0), amps(2.0)}),
        sensors({zone1(72.0), zone0(41.0)}, {amps(2.5), volts(12.1)}), // the same channels, listed the other way round
        sensors({zone1(74.0)}, {amps(3.0)}),                           // thermal_zone0 and in1 gone
    });
    auto sampler = sensorModelOver(std::move(probe));

    sampler->sample();
    const auto first = sampler->publication();
    const Platform::SensorDevice& chip = first->sensors.devices[0];
    const Platform::SensorDevice& thermal = first->sensors.devices[1];
    const std::string zone0Key = sensorKey(thermal, thermal.readings[0]);
    const std::string zone1Key = sensorKey(thermal, thermal.readings[1]);
    const std::string voltsKey = sensorKey(chip, chip.readings[0]);
    const std::string ampsKey = sensorKey(chip, chip.readings[1]);
    EXPECT_NE(zone0Key, zone1Key);
    EXPECT_NE(voltsKey, ampsKey);
    ASSERT_EQ(first->ranges.size(), 4U);
    ASSERT_EQ(first->histories.size(), 4U);
    EXPECT_EQ(first->ranges.at(zone0Key), (SensorRange{.min = 40.0, .max = 40.0, .samples = 1}));
    EXPECT_EQ(first->ranges.at(zone1Key), (SensorRange{.min = 70.0, .max = 70.0, .samples = 1}));
    EXPECT_EQ(first->ranges.at(voltsKey), (SensorRange{.min = 12.0, .max = 12.0, .samples = 1}));
    EXPECT_EQ(first->ranges.at(ampsKey), (SensorRange{.min = 2.0, .max = 2.0, .samples = 1}));
    // Every channel enters its own history, though they were all read at the same time.
    EXPECT_EQ(first->histories.at(zone0Key).values, std::vector<float>{40.0F});
    EXPECT_EQ(first->histories.at(zone1Key).values, std::vector<float>{70.0F});
    EXPECT_EQ(first->histories.at(voltsKey).values, std::vector<float>{12.0F});
    EXPECT_EQ(first->histories.at(ampsKey).values, std::vector<float>{2.0F});

    sampler->sample();
    sampler->sample();
    const auto latest = sampler->publication();
    ASSERT_EQ(latest->ranges.size(), 4U);
    EXPECT_EQ(latest->ranges.at(zone0Key), (SensorRange{.min = 40.0, .max = 41.0, .samples = 2}));
    EXPECT_EQ(latest->ranges.at(zone1Key), (SensorRange{.min = 70.0, .max = 74.0, .samples = 3}));
    EXPECT_EQ(latest->ranges.at(voltsKey), (SensorRange{.min = 12.0, .max = 12.1, .samples = 2}));
    EXPECT_EQ(latest->ranges.at(ampsKey), (SensorRange{.min = 2.0, .max = 3.0, .samples = 3}));
    EXPECT_EQ(latest->histories.at(zone0Key).values, (std::vector<float>{40.0F, 41.0F}));
    EXPECT_EQ(latest->histories.at(zone1Key).values, (std::vector<float>{70.0F, 72.0F, 74.0F}));
    EXPECT_EQ(latest->histories.at(voltsKey).values, (std::vector<float>{12.0F, 12.1F}));
    EXPECT_EQ(latest->histories.at(ampsKey).values, (std::vector<float>{2.0F, 2.5F, 3.0F}));
}

/// A full sensors read of @p devices (#2495).
[[nodiscard]] Platform::SensorsInfo scriptedSensors(std::vector<Platform::SensorDevice> devices)
{
    Platform::SensorsInfo info;
    info.available = true;
    info.listed = true;
    info.devices = std::move(devices);
    return info;
}

/// A device @p id named @p name with one temperature "temp1" reading @p value (#2495).
[[nodiscard]] Platform::SensorDevice scriptedChip(std::string name, std::string id, double value)
{
    return {.name = std::move(name),
            .id = std::move(id),
            .readings = {scriptedReading(Platform::SensorKind::Temperature, "Composite", "temp1", value)}};
}

// #2495: the kernel hands a freed hwmon number to the next device plugged in. The new device's sensor
// starts with its own range and history, not the unplugged one's, even with no read in between.
TEST(SystemInfoModelTest, ADeviceTakingAnotherOnesHwmonNumberStartsFresh)
{
    int reads = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->scriptSensors({
        scriptedSensors({scriptedChip("coretemp", "hwmon2", 50.0), scriptedChip("nvme", "hwmon5", 40.0)}),
        scriptedSensors({scriptedChip("coretemp", "hwmon2", 52.0), scriptedChip("nvme", "hwmon5", 55.0)}),
        // The NVMe enclosure unplugged and a Wi-Fi dongle plugged in, between two reads.
        scriptedSensors({scriptedChip("coretemp", "hwmon2", 51.0), scriptedChip("mt7921_phy0", "hwmon5", 30.0)}),
    });
    auto sensors = sensorModelOver(std::move(probe));

    sensors->sample();
    sensors->sample();
    const auto before = sensors->publication();
    const std::string coretemp = sensorKey(before->sensors.devices[0], before->sensors.devices[0].readings[0]);
    const std::string hwmon5 = sensorKey(before->sensors.devices[1], before->sensors.devices[1].readings[0]);
    EXPECT_EQ(before->ranges.at(hwmon5), (SensorRange{.min = 40.0, .max = 55.0, .samples = 2}));
    EXPECT_EQ(before->histories.at(hwmon5).values, (std::vector<float>{40.0F, 55.0F}));

    sensors->sample();
    const auto after = sensors->publication();
    ASSERT_EQ(after->sensors.devices[1].name, "mt7921_phy0");
    EXPECT_EQ(sensorKey(after->sensors.devices[1], after->sensors.devices[1].readings[0]), hwmon5) << "the same key";
    EXPECT_EQ(after->ranges.at(hwmon5), (SensorRange{.min = 30.0, .max = 30.0, .samples = 1}));
    EXPECT_EQ(after->histories.at(hwmon5).values, std::vector<float>{30.0F});
    // The device that stayed keeps its own.
    EXPECT_EQ(after->ranges.at(coretemp), (SensorRange{.min = 50.0, .max = 52.0, .samples = 3}));
    EXPECT_EQ(after->histories.at(coretemp).values, (std::vector<float>{50.0F, 52.0F, 51.0F}));
}

// #2495: a device gone from a full sensors list takes its range and history with it, so the maps don't
// keep every device ever seen; a read that couldn't list the devices drops nothing, and a device that
// only gained or lost the " #2" of its name among namesakes keeps its own.
TEST(SystemInfoModelTest, ADeviceGoneFromTheSensorsListIsForgotten)
{
    Platform::SensorsInfo unlisted;
    unlisted.available = true;
    unlisted.listed = false;
    int reads = 0;
    auto probe = std::make_unique<FakeSystemInfoProbe>(true, &reads);
    probe->scriptSensors({
        scriptedSensors({scriptedChip("nvme", "hwmon3", 35.0), scriptedChip("nvme #2", "hwmon5", 40.0)}),
        unlisted,
        scriptedSensors({scriptedChip("nvme", "hwmon5", 45.0)}), // hwmon3 unplugged: hwmon5 is "nvme" now
        scriptedSensors({scriptedChip("nvme", "hwmon3", 60.0), scriptedChip("nvme #2", "hwmon5", 42.0)}),
    });
    auto sensors = sensorModelOver(std::move(probe));

    sensors->sample();
    const auto first = sensors->publication();
    const std::string hwmon3 = sensorKey(first->sensors.devices[0], first->sensors.devices[0].readings[0]);
    const std::string hwmon5 = sensorKey(first->sensors.devices[1], first->sensors.devices[1].readings[0]);
    ASSERT_EQ(first->ranges.size(), 2U);

    sensors->sample(); // couldn't list the devices
    EXPECT_EQ(sensors->publication()->ranges.size(), 2U);
    EXPECT_EQ(sensors->publication()->histories.size(), 2U);

    sensors->sample();
    const auto unplugged = sensors->publication();
    EXPECT_FALSE(unplugged->ranges.contains(hwmon3));
    EXPECT_FALSE(unplugged->histories.contains(hwmon3));
    EXPECT_EQ(unplugged->ranges.at(hwmon5), (SensorRange{.min = 40.0, .max = 45.0, .samples = 2}));

    sensors->sample();
    const auto replugged = sensors->publication();
    EXPECT_EQ(replugged->ranges.at(hwmon3), (SensorRange{.min = 60.0, .max = 60.0, .samples = 1}));
    EXPECT_EQ(replugged->histories.at(hwmon3).values, std::vector<float>{60.0F});
    EXPECT_EQ(replugged->ranges.at(hwmon5), (SensorRange{.min = 40.0, .max = 45.0, .samples = 3}));
    EXPECT_EQ(replugged->histories.at(hwmon5).values, (std::vector<float>{40.0F, 45.0F, 42.0F}));
}

// #2340: a source without ids still keeps a voltage and a current that share a label apart.
TEST(SystemInfoModelTest, SensorKeyWithoutIdsStillTellsKindsApart)
{
    using Platform::SensorKind;
    const Platform::SensorDevice device{.name = "chip", .id = {}, .readings = {}};
    const Platform::SensorReading volts = scriptedReading(SensorKind::Voltage, "VDD_IN", {}, 12.0);
    const Platform::SensorReading amps = scriptedReading(SensorKind::Current, "VDD_IN", {}, 2.0);
    EXPECT_NE(sensorKey(device, volts), sensorKey(device, amps));
    // With ids, the device's and the reading's ids are the identity, not the name or label.
    const Platform::SensorDevice renamed{.name = "chip #2", .id = "hwmon4", .readings = {}};
    const Platform::SensorDevice same{.name = "chip", .id = "hwmon4", .readings = {}};
    const Platform::SensorReading relabelled = scriptedReading(SensorKind::Voltage, "VCORE", "in1", 1.0);
    const Platform::SensorReading labelled = scriptedReading(SensorKind::Voltage, "VDD_IN", "in1", 1.0);
    EXPECT_EQ(sensorKey(renamed, relabelled), sensorKey(same, labelled));
}

TEST(SystemInfoModelTest, SensorHistoryIsOrderedAndBounded)
{
    SensorHistory range;
    appendSensorHistory(range, 40.0, 100.0);
    appendSensorHistory(range, 45.0, 100.0); // not after the newest: ignored
    appendSensorHistory(range, 45.0, 99.0);
    ASSERT_EQ(range.values.size(), 1U);
    EXPECT_FLOAT_EQ(range.values[0], 40.0F);

    // At the shortest refresh interval, the history keeps exactly its capacity, oldest dropped first.
    constexpr double FASTEST = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 1000.0;
    std::size_t index = 1;
    while (index <= Sampling::SENSOR_HISTORY_CAPACITY + 10)
    {
        appendSensorHistory(range, 40.0 + static_cast<double>(index), 100.0 + (static_cast<double>(index) * FASTEST), FASTEST);
        ++index;
    }
    EXPECT_EQ(range.values.size(), Sampling::SENSOR_HISTORY_CAPACITY);
    EXPECT_EQ(range.times.size(), range.values.size());
    EXPECT_FLOAT_EQ(range.values.back(), 40.0F + static_cast<float>(Sampling::SENSOR_HISTORY_CAPACITY + 10));
    EXPECT_FLOAT_EQ(range.values.front(), 40.0F + 11.0F);
    EXPECT_LE(range.times.back() - range.times.front(), static_cast<double>(Sampling::HISTORY_SECONDS_DEFAULT));
}

TEST(SystemInfoModelTest, APauseBreaksTheSensorHistoryAndOldReadingsAgeOut)
{
    constexpr double REFRESH = static_cast<double>(Sampling::SYSTEM_INFO_SENSOR_REFRESH_MS) / 1000.0;
    SensorHistory range;
    appendSensorHistory(range, 40.0, 1000.0);
    appendSensorHistory(range, 41.0, 1000.0 + REFRESH);
    // The tab was hidden for a minute: a NaN break a refresh after the last reading, then the new one.
    appendSensorHistory(range, 50.0, 1060.0);
    ASSERT_EQ(range.values.size(), 4U);
    EXPECT_TRUE(std::isnan(range.values[2]));
    EXPECT_DOUBLE_EQ(range.times[2], 1000.0 + (2.0 * REFRESH));
    EXPECT_FLOAT_EQ(range.values[3], 50.0F);

    // Hidden longer than the window: everything before it ages out.
    const double later = 1060.0 + static_cast<double>(Sampling::HISTORY_SECONDS_DEFAULT) + 100.0;
    appendSensorHistory(range, 55.0, later);
    ASSERT_EQ(range.values.size(), 1U);
    EXPECT_FLOAT_EQ(range.values[0], 55.0F);
    EXPECT_DOUBLE_EQ(range.times[0], later);
}

} // namespace
} // namespace Domain
