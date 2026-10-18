#include "App/Panels/PowerStatusText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>

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
    return text.contains(ICON_FA_PLUG);
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

TEST(PowerStatusTextTest, AnUnreportedChargeIsADashNotMinusOne)
{
    // #2408: chargePercent -1 is "no reading", shown as a dash in every state
    for (const auto setState : {+[](Domain::PowerStatus&) {},
                                +[](Domain::PowerStatus& p) { p.isCharging = true; },
                                +[](Domain::PowerStatus& p) { p.isNotCharging = true; },
                                +[](Domain::PowerStatus& p) { p.isDischarging = true; }})
    {
        auto power = battery(-1, false);
        setState(power);
        const std::string text = batteryHeaderStatus(power);
        EXPECT_EQ(text.find("-1"), std::string::npos) << text;
        EXPECT_NE(text.find(NO_CHARGE_TEXT), std::string::npos) << text;
    }
    EXPECT_EQ(formatChargePercent(42), "42%");
}

TEST(PowerStatusTextTest, TheNoChargeDashIsAnEmDashInUtf8)
{
    // #2529: the literal was double-encoded (C3 A2 C2 80 C2 94: U+00E2 plus two C1 controls), which
    // a find() of the constant in the text it built could not catch. Pin the exact bytes.
    EXPECT_EQ(NO_CHARGE_TEXT, "\xE2\x80\x94");
    EXPECT_EQ(formatChargePercent(-1), "\xE2\x80\x94");
}

TEST(PowerStatusTextTest, TheBatteryBarIsHatchedNotStaleWithoutACharge)
{
    // #2529: the smoothed charge still eases toward the last reading, so without a reading the bar
    // must not draw it: a dash and a hatched (unavailable) bar instead.
    const ImVec4 color{0.1F, 0.2F, 0.3F, 1.0F};
    const auto noReading = batteryNowBar(battery(-1, false), 73.0, "Battery", color);
    EXPECT_FALSE(noReading.available);
    EXPECT_DOUBLE_EQ(noReading.value01, 0.0);
    EXPECT_EQ(noReading.valueText, "\xE2\x80\x94");
    EXPECT_EQ(noReading.label, "Battery");

    const auto reading = batteryNowBar(battery(52, false), 52.0, "Battery", color);
    EXPECT_TRUE(reading.available);
    EXPECT_GT(reading.value01, 0.0);
    EXPECT_LE(reading.value01, 1.0);
    EXPECT_FALSE(reading.valueText.contains('-')) << reading.valueText;
    EXPECT_TRUE(reading.valueText.contains("52")) << reading.valueText;
}

} // namespace
} // namespace App::Detail
