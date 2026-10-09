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

    const SystemInfoModel stub(std::make_unique<Platform::UnsupportedSystemInfoProbe>());
    EXPECT_FALSE(stub.capabilities().hasOs);
}

TEST(SystemInfoModelTest, RequiresAProbe)
{
    EXPECT_THROW(SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe>{}), std::invalid_argument);
}

} // namespace
} // namespace Domain
