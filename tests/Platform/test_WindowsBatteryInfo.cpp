/// @file test_WindowsBatteryInfo.cpp
/// @brief Platform::Windows::readBatteryInfo() (#1523) and WindowsPowerProbe's battery details,
/// against fake battery device calls: a normal battery, relative-capacity units, no battery, a
/// failed IOCTL, and the device handle closed exactly once.

#include "Platform/PowerTypes.h"
#include "Platform/Windows/WindowsBatteryInfo.h"
#include "Platform/Windows/WindowsPowerProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Platform::Windows
{
namespace
{

/// The answers the fakes give, set per test.
struct FakeBattery
{
    std::vector<std::wstring> paths{LR"(\\?\acpi#pnp0c0a#1#{72631e54-78a4-11d0-bcf7-00aa00b7b32a})"};
    bool failOpen = false;
    ULONG tag = 7;
    bool failInformation = false;
    ULONG capabilities = BATTERY_SYSTEM_BATTERY;
    ULONG designed = 52'600;
    ULONG fullCharged = 47'340;
    ULONG cycles = 123;
    const char* chemistry = "LION";
    std::wstring manufacturer = L"Contoso";
    std::wstring model = L"5B10W51";
    int opens = 0;
    int closes = 0;
    int informationQueries = 0;
};

FakeBattery& fake()
{
    static FakeBattery instance;
    return instance;
}

HANDLE fakeDevice()
{
    static int token = 0;
    return static_cast<HANDLE>(&token);
}

std::vector<std::wstring> fakeEnumerate()
{
    return fake().paths;
}

HANDLE WINAPI fakeCreateFile(LPCWSTR /*path*/,
                             DWORD /*access*/,
                             DWORD /*share*/,
                             LPSECURITY_ATTRIBUTES /*sa*/,
                             DWORD /*disposition*/,
                             DWORD /*flags*/,
                             HANDLE /*tmpl*/)
{
    if (fake().failOpen)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    ++fake().opens;
    return fakeDevice();
}

BOOL WINAPI fakeDeviceIoControl(
    HANDLE device, DWORD code, LPVOID in, DWORD /*inBytes*/, LPVOID out, DWORD outBytes, LPDWORD returned, LPOVERLAPPED /*overlapped*/)
{
    EXPECT_EQ(device, fakeDevice());
    if (code == IOCTL_BATTERY_QUERY_TAG)
    {
        std::memcpy(out, &fake().tag, sizeof(ULONG));
        *returned = sizeof(ULONG);
        return TRUE;
    }
    EXPECT_EQ(code, static_cast<DWORD>(IOCTL_BATTERY_QUERY_INFORMATION));
    ++fake().informationQueries;
    BATTERY_QUERY_INFORMATION query{};
    std::memcpy(&query, in, sizeof(query));
    EXPECT_EQ(query.BatteryTag, fake().tag);
    if (fake().failInformation)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    if (query.InformationLevel == BatteryInformation)
    {
        BATTERY_INFORMATION info{};
        info.Capabilities = fake().capabilities;
        info.DesignedCapacity = fake().designed;
        info.FullChargedCapacity = fake().fullCharged;
        info.CycleCount = fake().cycles;
        std::memcpy(static_cast<UCHAR*>(info.Chemistry), fake().chemistry, std::min<std::size_t>(std::strlen(fake().chemistry), 4));
        std::memcpy(out, &info, sizeof(info));
        *returned = sizeof(info);
        return TRUE;
    }
    // Only the two names are asked for: never the serial number
    EXPECT_TRUE(query.InformationLevel == BatteryManufactureName || query.InformationLevel == BatteryDeviceName);
    const std::wstring& name = (query.InformationLevel == BatteryManufactureName) ? fake().manufacturer : fake().model;
    const auto bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    EXPECT_LE(bytes, outBytes);
    std::memcpy(out, name.c_str(), bytes);
    *returned = bytes;
    return TRUE;
}

BOOL WINAPI fakeCloseHandle(HANDLE handle)
{
    EXPECT_EQ(handle, fakeDevice());
    ++fake().closes;
    return TRUE;
}

class WindowsBatteryInfoTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeBattery{};
    }

    static BatteryDeviceFunctions fakes()
    {
        return {.enumerateBatteries = &fakeEnumerate,
                .createFile = &fakeCreateFile,
                .deviceIoControl = &fakeDeviceIoControl,
                .closeHandle = &fakeCloseHandle};
    }
};

TEST_F(WindowsBatteryInfoTest, NormalBatteryReportsEveryField)
{
    const BatteryInfo info = readBatteryInfo(fakes());
    EXPECT_TRUE(info.found);
    EXPECT_FALSE(info.relativeCapacity);
    EXPECT_DOUBLE_EQ(info.designWh, 52.6);
    EXPECT_DOUBLE_EQ(info.fullChargeWh, 47.34);
    EXPECT_EQ(info.healthPercent, 90);
    EXPECT_EQ(info.cycleCount, 123U);
    EXPECT_EQ(info.technology, "Li-ion");
    EXPECT_EQ(info.manufacturer, "Contoso");
    EXPECT_EQ(info.model, "5B10W51");
    EXPECT_EQ(fake().opens, 1);
    EXPECT_EQ(fake().closes, 1);
}

TEST_F(WindowsBatteryInfoTest, RelativeCapacityUnitsAreNotReportedAsWattHours)
{
    fake().capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
    fake().designed = 100;
    fake().fullCharged = 92;
    const BatteryInfo info = readBatteryInfo(fakes());
    EXPECT_TRUE(info.found);
    EXPECT_TRUE(info.relativeCapacity);
    EXPECT_DOUBLE_EQ(info.designWh, 0.0);
    EXPECT_DOUBLE_EQ(info.fullChargeWh, 0.0);
    EXPECT_EQ(info.healthPercent, -1);
    EXPECT_EQ(info.cycleCount, 123U); // The rest is still reported
    EXPECT_EQ(info.technology, "Li-ion");
}

TEST_F(WindowsBatteryInfoTest, NoBatteryOrAFailedCallFindsNothingAndClosesWhatItOpened)
{
    fake().paths.clear(); // No battery device
    EXPECT_FALSE(readBatteryInfo(fakes()).found);

    fake() = FakeBattery{};
    fake().tag = 0; // A battery slot with no battery in it
    EXPECT_FALSE(readBatteryInfo(fakes()).found);
    EXPECT_EQ(fake().informationQueries, 0);

    fake() = FakeBattery{};
    fake().failOpen = true;
    EXPECT_FALSE(readBatteryInfo(fakes()).found);
    EXPECT_EQ(fake().closes, 0); // INVALID_HANDLE_VALUE is never closed

    fake() = FakeBattery{};
    fake().failInformation = true; // IOCTL_BATTERY_QUERY_INFORMATION fails
    EXPECT_FALSE(readBatteryInfo(fakes()).found);
    EXPECT_EQ(fake().opens, 1);
    EXPECT_EQ(fake().closes, 1);
}

TEST_F(WindowsBatteryInfoTest, ProbeCapabilitiesFollowWhatTheBatteryReported)
{
    const PowerCapabilities normal = WindowsPowerProbe(fakes()).capabilities();
    EXPECT_TRUE(normal.hasDesignCapacity);
    EXPECT_TRUE(normal.hasHealthPercent);
    EXPECT_TRUE(normal.hasCycleCount);
    EXPECT_TRUE(normal.hasTechnology);
    EXPECT_FALSE(normal.hasChargeCapacity); // No live capacity from GetSystemPowerStatus

    fake().capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
    fake().cycles = 0; // The driver's "no cycle counter"
    const PowerCapabilities relative = WindowsPowerProbe(fakes()).capabilities();
    EXPECT_FALSE(relative.hasDesignCapacity);
    EXPECT_FALSE(relative.hasHealthPercent);
    EXPECT_FALSE(relative.hasCycleCount);
    EXPECT_TRUE(relative.hasTechnology);

    fake() = FakeBattery{};
    fake().failInformation = true;
    const PowerCapabilities failed = WindowsPowerProbe(fakes()).capabilities();
    EXPECT_FALSE(failed.hasDesignCapacity || failed.hasHealthPercent || failed.hasCycleCount || failed.hasTechnology);
}

TEST_F(WindowsBatteryInfoTest, ProbeReadsTheStaticFactsOnceNotPerSample)
{
    WindowsPowerProbe probe(fakes());
    const int afterConstruction = fake().informationQueries;
    EXPECT_GT(afterConstruction, 0);
    for (int i = 0; i < 5; ++i)
    {
        const PowerCounters counters = probe.read();
        if (counters.state != BatteryState::NotPresent) // GetSystemPowerStatus is the real one
        {
            EXPECT_DOUBLE_EQ(counters.chargeDesignWh, 52.6);
            EXPECT_EQ(counters.technology, "Li-ion");
            EXPECT_EQ(counters.manufacturer, "Contoso");
        }
    }
    // Five samples on an unchanged power source: no further device queries
    EXPECT_EQ(fake().informationQueries, afterConstruction);
}

} // namespace
} // namespace Platform::Windows
