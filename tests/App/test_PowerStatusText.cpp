#include "App/Panels/PowerStatusText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>

#include <string>

namespace App::Detail
{
namespace
{

[[nodiscard]] Domain::PowerStatus battery(int charge, bool onAc)
{
    Domain::PowerStatus power;
    power.hasBattery = true;
    power.chargePercent = charge;
    power.isOnAc = onAc;
    return power;
}

[[nodiscard]] bool showsPlug(const std::string& text)
{
    return text.find(ICON_FA_PLUG) != std::string::npos;
}

TEST(PowerStatusTextTest, PlugFollowsTheAdapterNotTheBatteryState)
{
    // A stale Discharging while the adapter is online shows the plug and no time left; Full with
    // the adapter offline shows none (#1109).
    auto staleDischarging = battery(60, true);
    staleDischarging.isDischarging = true;
    staleDischarging.timeToEmptySec = 3600;
    const std::string onAc = batteryHeaderStatus(staleDischarging);
    EXPECT_TRUE(showsPlug(onAc)) << onAc;
    EXPECT_EQ(onAc.find("left"), std::string::npos) << onAc;

    auto fullOffline = battery(100, false);
    fullOffline.isFull = true;
    EXPECT_FALSE(showsPlug(batteryHeaderStatus(fullOffline)));
}

TEST(PowerStatusTextTest, DischargingOnBatteryShowsTimeLeft)
{
    auto power = battery(40, false);
    power.isDischarging = true;
    power.timeToEmptySec = 5400;
    const std::string text = batteryHeaderStatus(power);
    EXPECT_FALSE(showsPlug(text)) << text;
    EXPECT_NE(text.find("40% (1:30 left)"), std::string::npos) << text;
}

TEST(PowerStatusTextTest, NotChargingShowsItsRealCharge)
{
    auto power = battery(80, true);
    power.isNotCharging = true;
    const std::string text = batteryHeaderStatus(power);
    EXPECT_TRUE(showsPlug(text)) << text;
    EXPECT_NE(text.find("80% (not charging)"), std::string::npos) << text;
}

TEST(PowerStatusTextTest, ChargingShowsTimeToFull)
{
    auto power = battery(50, true);
    power.isCharging = true;
    power.timeToFullSec = 1800;
    const std::string text = batteryHeaderStatus(power);
    EXPECT_NE(text.find(ICON_FA_BOLT), std::string::npos) << text;
    EXPECT_NE(text.find("50% (0:30 to full)"), std::string::npos) << text;
}

} // namespace
} // namespace App::Detail
