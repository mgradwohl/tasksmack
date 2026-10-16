/// @file UnsupportedSystemInfoProbe.h
/// @brief An ISystemInfoProbe with no facts, for tests of the "not available" paths (#2169)

#pragma once

#include "Platform/ISystemInfoProbe.h"

namespace Mocks
{

/// A platform without a System Information implementation: no facts, and hasOs false so the UI says so.
/// Every platform has one, so this lives with the tests that need the empty answer (#2169).
class UnsupportedSystemInfoProbe final : public Platform::ISystemInfoProbe
{
  public:
    [[nodiscard]] Platform::SystemInfoCapabilities capabilities() const override
    {
        return {.hasOs = false, .unavailableReason = "System information isn't available on this platform"};
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
        return {};
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

} // namespace Mocks
