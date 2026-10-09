/// @file test_SystemInfoModel.cpp
/// @brief Domain::SystemInfoModel (#1399): nothing is read until read() is called, each read publishes
/// a new immutable snapshot, and a probe without OS facts is never asked for them.

#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace Domain
{
namespace
{

class FakeSystemInfoProbe final : public Platform::ISystemInfoProbe
{
  public:
    FakeSystemInfoProbe(bool hasOs, int* reads) : m_HasOs(hasOs), m_Reads(reads)
    {}

    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = m_HasOs, .unavailableReason = m_HasOs ? std::string{} : std::string{"not here"}};
    }

    [[nodiscard]] Platform::OsInfo readOs() override
    {
        ++*m_Reads;
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
        return info;
    }

    [[nodiscard]] Platform::DevicesInfo readDevices() override
    {
        Platform::DevicesInfo info;
        info.available = true;
        info.pciRead = true;
        return info;
    }

    [[nodiscard]] Platform::NetworkAdaptersInfo readNetworkAdapters() override
    {
        Platform::NetworkAdaptersInfo info;
        info.available = true;
        info.gatewayV4 = "192.168.1.1";
        return info;
    }

  private:
    bool m_HasOs;
    int* m_Reads;
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
    EXPECT_EQ(first->adapters.gatewayV4, "192.168.1.1");
    EXPECT_GT(first->readAtUnixSeconds, 0U);

    model.read(); // Refresh
    EXPECT_EQ(model.version(), 2U);
    EXPECT_EQ(model.snapshot()->os.build, "26100.2");
    EXPECT_EQ(first->os.build, "26100.1"); // the earlier snapshot is unchanged
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
    EXPECT_FALSE(model.snapshot()->adapters.available);

    const SystemInfoModel stub(std::make_unique<Platform::UnsupportedSystemInfoProbe>());
    EXPECT_FALSE(stub.capabilities().hasOs);
}

TEST(SystemInfoModelTest, RequiresAProbe)
{
    EXPECT_THROW(SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe>{}), std::invalid_argument);
}

} // namespace
} // namespace Domain
