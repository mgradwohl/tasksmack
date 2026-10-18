/// @file test_WindowsBatteryInfo.cpp
/// @brief Platform::Windows::readBatteryInfo() (#1523) and WindowsPowerProbe's battery details,
/// against fake battery device calls: a normal battery, two batteries summed (#2151),
/// relative-capacity units, no battery, a failed IOCTL, and the device handle closed exactly once;
/// and the probe's capabilities following a battery inserted, removed or answering late (#2413).

#include "Platform/PowerTypes.h"
#include "Platform/Windows/WindowsBatteryInfo.h"
#include "Platform/Windows/WindowsPowerProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
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
    // A second battery (#2151), answering when the second path is the one opened
    ULONG secondDesigned = 72'000;
    ULONG secondFullCharged = 43'200;
    ULONG secondCycles = 310;
    std::wstring secondModel = L"External";
    std::size_t openedIndex = 0; ///< Which path the open device is
    int opens = 0;
    int closes = 0;
    int informationQueries = 0;
    // GetSystemPowerStatus (#1395): on battery, discharging, 80% with an hour left
    bool failPowerStatus = false;
    BYTE acLineStatus = 0;
    BYTE batteryFlag = 1; // BATTERY_FLAG_HIGH
    BYTE batteryLifePercent = 80;
    DWORD batteryLifeTime = 3'600;
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

HANDLE WINAPI fakeCreateFile(
    LPCWSTR path, DWORD /*access*/, DWORD /*share*/, LPSECURITY_ATTRIBUTES /*sa*/, DWORD /*disposition*/, DWORD /*flags*/, HANDLE /*tmpl*/)
{
    if (fake().failOpen)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    ++fake().opens;
    const auto opened = std::ranges::find(fake().paths, std::wstring_view(path));
    EXPECT_NE(opened, fake().paths.end());
    fake().openedIndex = static_cast<std::size_t>(opened - fake().paths.begin());
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
    const bool second = fake().openedIndex == 1;
    if (query.InformationLevel == BatteryInformation)
    {
        BATTERY_INFORMATION info{};
        info.Capabilities = fake().capabilities;
        info.DesignedCapacity = second ? fake().secondDesigned : fake().designed;
        info.FullChargedCapacity = second ? fake().secondFullCharged : fake().fullCharged;
        info.CycleCount = second ? fake().secondCycles : fake().cycles;
        std::memcpy(static_cast<UCHAR*>(info.Chemistry), fake().chemistry, std::min<std::size_t>(std::strlen(fake().chemistry), 4));
        std::memcpy(out, &info, sizeof(info));
        *returned = sizeof(info);
        return TRUE;
    }
    // Only the two names are asked for: never the serial number
    EXPECT_TRUE(query.InformationLevel == BatteryManufactureName || query.InformationLevel == BatteryDeviceName);
    const std::wstring& model = second ? fake().secondModel : fake().model;
    const std::wstring& name = (query.InformationLevel == BatteryManufactureName) ? fake().manufacturer : model;
    const auto bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    EXPECT_LE(bytes, outBytes);
    std::memcpy(out, name.c_str(), bytes);
    *returned = bytes;
    return TRUE;
}

BOOL WINAPI fakeGetSystemPowerStatus(LPSYSTEM_POWER_STATUS status)
{
    if (fake().failPowerStatus)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    *status = SYSTEM_POWER_STATUS{};
    status->ACLineStatus = fake().acLineStatus;
    status->BatteryFlag = fake().batteryFlag;
    status->BatteryLifePercent = fake().batteryLifePercent;
    status->BatteryLifeTime = fake().batteryLifeTime;
    status->BatteryFullLifeTime = 0xFFFFFFFF;
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

TEST_F(WindowsBatteryInfoTest, TwoBatteriesAreSummedLikeTheChargePercent)
{
    // #2151: a 24 Wh internal pack at 95% and a 72 Wh external pack at 60%. The charge percent
    // covers both, so the capacities and health must too, not just the first pack's.
    fake().paths.emplace_back(LR"(\\?\acpi#pnp0c0a#2#{72631e54-78a4-11d0-bcf7-00aa00b7b32a})");
    fake().designed = 24'000;
    fake().fullCharged = 22'800;
    fake().cycles = 40;
    const BatteryInfo info = readBatteryInfo(fakes());
    EXPECT_TRUE(info.found);
    EXPECT_DOUBLE_EQ(info.designWh, 96.0);
    EXPECT_DOUBLE_EQ(info.fullChargeWh, 66.0);
    EXPECT_EQ(info.healthPercent, 69); // 66 / 96, not the first pack's 95
    EXPECT_EQ(info.cycleCount, 310U);  // The most-cycled pack
    EXPECT_EQ(info.model, "5B10W51");  // The names stay the first battery's
    EXPECT_EQ(fake().opens, 2);
    EXPECT_EQ(fake().closes, 2);
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
    const PowerCapabilities normal = WindowsPowerProbe(fakes(), &fakeGetSystemPowerStatus).capabilities();
    EXPECT_TRUE(normal.hasDesignCapacity);
    EXPECT_TRUE(normal.hasHealthPercent);
    EXPECT_TRUE(normal.hasCycleCount);
    EXPECT_TRUE(normal.hasTechnology);
    EXPECT_FALSE(normal.hasChargeCapacity); // No live capacity from GetSystemPowerStatus

    fake().capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
    fake().cycles = 0; // The driver's "no cycle counter"
    const PowerCapabilities relative = WindowsPowerProbe(fakes(), &fakeGetSystemPowerStatus).capabilities();
    EXPECT_FALSE(relative.hasDesignCapacity);
    EXPECT_FALSE(relative.hasHealthPercent);
    EXPECT_FALSE(relative.hasCycleCount);
    EXPECT_TRUE(relative.hasTechnology);

    fake() = FakeBattery{};
    fake().failInformation = true;
    const PowerCapabilities failed = WindowsPowerProbe(fakes(), &fakeGetSystemPowerStatus).capabilities();
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

// With GetSystemPowerStatus faked too (#1395): a laptop on any machine, and the call failing.

TEST_F(WindowsBatteryInfoTest, ProbeOnBatteryReportsTheChargeAndTheStaticFacts)
{
    WindowsPowerProbe probe(fakes(), &fakeGetSystemPowerStatus);
    const PowerCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.hasBattery);
    EXPECT_TRUE(caps.hasChargePercent);
    EXPECT_TRUE(caps.hasTimeEstimates);

    const PowerCounters counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_FALSE(counters.isOnAc);
    EXPECT_EQ(counters.chargePercent, 80);
    EXPECT_DOUBLE_EQ(counters.chargeDesignWh, 52.6);
    EXPECT_DOUBLE_EQ(counters.chargeFullWh, 47.34);
    EXPECT_EQ(counters.healthPercent, 90);
    EXPECT_EQ(counters.cycleCount, 123U);
    EXPECT_EQ(counters.technology, "Li-ion");
    EXPECT_EQ(counters.manufacturer, "Contoso");
    EXPECT_EQ(counters.model, "5B10W51");
}

TEST_F(WindowsBatteryInfoTest, ProbeRereadsTheStaticFactsWhenThePowerSourceChanges)
{
    WindowsPowerProbe probe(fakes(), &fakeGetSystemPowerStatus);
    (void) probe.read();
    const int onBattery = fake().informationQueries;

    fake().acLineStatus = 1; // Plugged in: the full-charge capacity is recalibrated across a charge
    fake().batteryFlag = BATTERY_FLAG_CHARGING;
    fake().fullCharged = 50'000;
    PowerCounters counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Charging);
    EXPECT_GT(fake().informationQueries, onBattery);
    EXPECT_DOUBLE_EQ(counters.chargeFullWh, 50.0);

    const int onAc = fake().informationQueries;
    counters = probe.read();
    EXPECT_EQ(fake().informationQueries, onAc) << "the same source again: no re-read";
    EXPECT_DOUBLE_EQ(counters.chargeFullWh, 50.0);
}

TEST_F(WindowsBatteryInfoTest, ProbeWithNoBatteryReadsNoDeviceFacts)
{
    fake().acLineStatus = 1;
    fake().batteryFlag = BATTERY_FLAG_NO_BATTERY;
    WindowsPowerProbe probe(fakes(), &fakeGetSystemPowerStatus);
    EXPECT_FALSE(probe.capabilities().hasBattery);
    const int afterConstruction = fake().informationQueries;
    const PowerCounters counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::NotPresent);
    EXPECT_DOUBLE_EQ(counters.chargeDesignWh, 0.0);
    EXPECT_TRUE(counters.manufacturer.empty());
    EXPECT_EQ(fake().informationQueries, afterConstruction);
}

TEST_F(WindowsBatteryInfoTest, ProbeOffersABatteryInsertedAfterStart)
{
    // #2413: started with the battery out, then the battery is inserted
    fake().acLineStatus = 1;
    fake().batteryFlag = BATTERY_FLAG_NO_BATTERY;
    fake().paths.clear();
    WindowsPowerProbe probe(fakes(), &fakeGetSystemPowerStatus);
    EXPECT_FALSE(probe.capabilities().hasBattery);
    EXPECT_FALSE(probe.capabilities().hasDesignCapacity);

    fake().batteryFlag = BATTERY_FLAG_CHARGING;
    fake().batteryLifePercent = 40;
    fake().paths = FakeBattery{}.paths;
    const PowerCounters counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Charging);
    EXPECT_DOUBLE_EQ(counters.chargeDesignWh, 52.6) << "the new battery is read at once, not in five minutes";
    const PowerCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.hasBattery);
    EXPECT_TRUE(caps.hasChargePercent);
    EXPECT_TRUE(caps.hasDesignCapacity);
    EXPECT_TRUE(caps.hasHealthPercent);
    EXPECT_TRUE(caps.hasCycleCount);
    EXPECT_TRUE(caps.hasTechnology);

    // And removed again: no battery and none of its details, though the device last answered
    fake().batteryFlag = BATTERY_FLAG_NO_BATTERY;
    EXPECT_EQ(probe.read().state, BatteryState::NotPresent);
    const PowerCapabilities removed = probe.capabilities();
    EXPECT_FALSE(removed.hasBattery);
    EXPECT_FALSE(removed.hasChargePercent || removed.hasTimeEstimates);
    EXPECT_FALSE(removed.hasDesignCapacity || removed.hasHealthPercent || removed.hasCycleCount || removed.hasTechnology);
}

TEST_F(WindowsBatteryInfoTest, ProbeOffersDetailsTheBatteryFirstAnsweredLate)
{
    // #2413: the battery driver doesn't answer yet at start (right after resume, say)
    fake().failInformation = true;
    WindowsPowerProbe probe(fakes(), &fakeGetSystemPowerStatus);
    EXPECT_TRUE(probe.capabilities().hasBattery);
    EXPECT_FALSE(probe.capabilities().hasDesignCapacity);

    // It answers by the next re-read (here a change of power source; the periodic one works alike)
    fake().failInformation = false;
    fake().acLineStatus = 1;
    fake().batteryFlag = BATTERY_FLAG_CHARGING;
    (void) probe.read();
    const PowerCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.hasDesignCapacity) << "not 'Not reported' for the whole session";
    EXPECT_TRUE(caps.hasHealthPercent);
    EXPECT_TRUE(caps.hasCycleCount);
    EXPECT_TRUE(caps.hasTechnology);
}

TEST_F(WindowsBatteryInfoTest, FailedPowerStatusIsNoBatteryAndAnUnknownState)
{
    fake().failPowerStatus = true;
    WindowsPowerProbe probe(fakes(), &fakeGetSystemPowerStatus);
    const PowerCapabilities caps = probe.capabilities();
    EXPECT_FALSE(caps.hasBattery);
    EXPECT_FALSE(caps.hasChargePercent);
    EXPECT_FALSE(caps.hasTimeEstimates);

    const int afterConstruction = fake().informationQueries;
    const PowerCounters counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Unknown);
    EXPECT_DOUBLE_EQ(counters.chargeDesignWh, 0.0) << "a failed read reports no battery facts";
    EXPECT_EQ(fake().informationQueries, afterConstruction);
}

} // namespace
} // namespace Platform::Windows
