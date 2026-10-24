/// @file test_WindowsStorageSmartMath.cpp
/// @brief An ATA drive's SMART pages as plain bytes (#2588): the 30-entry attribute table (ids, the
/// little-endian flags word, current and worst values, the 48-bit little-endian raw value), the thresholds
/// page, SMART RETURN STATUS's task-file signatures, IDENTIFY DEVICE's SMART words, the "failing now" rule
/// for pre-fail attributes, and the AtaHealth the Storage section shows for the same facts Linux gets from
/// udisks2 (#1631): failing, bad sectors (reallocated plus pending), power-on hours and the temperature
/// from attribute 194 or 190. Built on every platform (#1133).

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsStorageSmartMath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Platform
{
namespace
{

namespace Smart = AtaSmartMath;

using Page = std::array<std::byte, Smart::PAGE_BYTES>;

/// One attribute entry as a drive stores it, written into @p page's @p slot.
void putAttribute(
    Page& page, std::size_t slot, std::uint8_t id, std::uint16_t flags, std::uint8_t current, std::uint8_t worst, std::uint64_t raw)
{
    const std::size_t at = Smart::ATTRIBUTE_TABLE_OFFSET + (slot * Smart::ATTRIBUTE_BYTES);
    page.at(at) = std::byte{id};
    page.at(at + 1) = static_cast<std::byte>(flags & 0xFFU);
    page.at(at + 2) = static_cast<std::byte>(flags >> 8U);
    page.at(at + 3) = std::byte{current};
    page.at(at + 4) = std::byte{worst};
    std::size_t index = 0;
    while (index < Smart::RAW_VALUE_BYTES)
    {
        page.at(at + 5 + index) = static_cast<std::byte>((raw >> (8U * index)) & 0xFFU);
        ++index;
    }
}

/// One thresholds entry: the id and its threshold.
void putThreshold(Page& page, std::size_t slot, std::uint8_t id, std::uint8_t threshold)
{
    const std::size_t at = Smart::ATTRIBUTE_TABLE_OFFSET + (slot * Smart::ATTRIBUTE_BYTES);
    page.at(at) = std::byte{id};
    page.at(at + 1) = std::byte{threshold};
}

constexpr std::uint16_t PREFAIL_FLAGS = 0x0033; // pre-fail, online, performance, error rate
constexpr std::uint16_t OLD_AGE_FLAGS = 0x0032;

/// A healthy desktop drive's page: 5, 9, 194 and 197 in slots with gaps, as drives lay them out.
[[nodiscard]] Page healthyPage()
{
    Page page{};
    page[0] = std::byte{0x10}; // the version word
    putAttribute(page, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, PREFAIL_FLAGS, 100, 100, 0);
    putAttribute(page, 2, Smart::ATTRIBUTE_POWER_ON_HOURS, OLD_AGE_FLAGS, 75, 75, 18'340);
    putAttribute(
        page, 5, Smart::ATTRIBUTE_TEMPERATURE, OLD_AGE_FLAGS, 35, 48, 0x001E'001B'0023); // 35 C now, min 27, max 30 in the high bytes
    putAttribute(page, 9, Smart::ATTRIBUTE_PENDING_SECTORS, OLD_AGE_FLAGS, 200, 200, 0);
    return page;
}

[[nodiscard]] std::vector<Smart::Attribute> attributesOf(const Page& page)
{
    return Smart::parseAttributes(page).value_or(std::vector<Smart::Attribute>{});
}

TEST(WindowsStorageSmartMathTest, ParsesTheAttributeTableInOrderSkippingEmptySlots)
{
    const std::vector<Smart::Attribute> attributes = attributesOf(healthyPage());
    ASSERT_EQ(attributes.size(), 4U);
    EXPECT_EQ(attributes[0].id, Smart::ATTRIBUTE_REALLOCATED_SECTORS);
    EXPECT_EQ(attributes[0].flags, PREFAIL_FLAGS);
    EXPECT_TRUE(attributes[0].preFail());
    EXPECT_EQ(attributes[0].current, 100);
    EXPECT_EQ(attributes[0].worst, 100);
    EXPECT_EQ(attributes[0].raw, 0U);
    EXPECT_EQ(attributes[1].id, Smart::ATTRIBUTE_POWER_ON_HOURS);
    EXPECT_FALSE(attributes[1].preFail());
    EXPECT_EQ(attributes[1].raw, 18'340U);
    EXPECT_EQ(attributes[2].id, Smart::ATTRIBUTE_TEMPERATURE);
    EXPECT_EQ(attributes[2].current, 35);
    EXPECT_EQ(attributes[2].worst, 48);
    EXPECT_EQ(attributes[2].raw, 0x001E'001B'0023U);
    EXPECT_EQ(attributes[3].id, Smart::ATTRIBUTE_PENDING_SECTORS);
}

TEST(WindowsStorageSmartMathTest, FlagsAndRawValueAreLittleEndian)
{
    Page page{};
    putAttribute(page, 0, Smart::ATTRIBUTE_CRC_ERRORS, 0x0102, 1, 2, 0xFEDC'BA98'7654);
    const std::size_t at = Smart::ATTRIBUTE_TABLE_OFFSET;
    // The bytes as written: id, flags low then high, current, worst, raw low byte first.
    EXPECT_EQ(page[at], std::byte{199});
    EXPECT_EQ(page[at + 1], std::byte{0x02});
    EXPECT_EQ(page[at + 2], std::byte{0x01});
    EXPECT_EQ(page[at + 5], std::byte{0x54});
    EXPECT_EQ(page[at + 10], std::byte{0xFE});
    page[at + 11] = std::byte{0xFF}; // the vendor byte is not part of the raw value
    const std::vector<Smart::Attribute> attributes = attributesOf(page);
    ASSERT_EQ(attributes.size(), 1U);
    EXPECT_EQ(attributes[0].flags, 0x0102);
    EXPECT_EQ(attributes[0].raw, 0xFEDC'BA98'7654U);
    EXPECT_EQ(Smart::littleEndian(std::span(page).subspan(at + 5, 2)), 0x7654U);
}

TEST(WindowsStorageSmartMathTest, AShortPageIsMalformed)
{
    const Page page = healthyPage();
    constexpr std::size_t TABLE_END = Smart::ATTRIBUTE_TABLE_OFFSET + Smart::ATTRIBUTE_TABLE_BYTES;
    EXPECT_FALSE(Smart::parseAttributes(std::span(page).first(TABLE_END - 1)).has_value());
    EXPECT_TRUE(Smart::parseAttributes(std::span(page).first(TABLE_END)).has_value());
    EXPECT_FALSE(Smart::parseThresholds(std::span(page).first(TABLE_END - 1)).has_value());
    EXPECT_TRUE(Smart::parseThresholds(std::span(page).first(TABLE_END)).has_value());
    EXPECT_FALSE(Smart::parseAttributes({}).has_value());
    // A full page with every slot empty is a drive with no attributes, not a malformed page.
    EXPECT_TRUE(Smart::parseAttributes(Page{}).value_or(std::vector<Smart::Attribute>{{}}).empty());
}

TEST(WindowsStorageSmartMathTest, ParsesThresholdsByAttributeId)
{
    Page page{};
    putThreshold(page, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, 10);
    putThreshold(page, 3, Smart::ATTRIBUTE_POWER_ON_HOURS, 0);
    putThreshold(page, 29, Smart::ATTRIBUTE_CRC_ERRORS, 0xFE);
    const Smart::Thresholds thresholds = Smart::parseThresholds(page).value_or(Smart::Thresholds{});
    EXPECT_EQ(thresholds[Smart::ATTRIBUTE_REALLOCATED_SECTORS], 10);
    EXPECT_EQ(thresholds[Smart::ATTRIBUTE_POWER_ON_HOURS], 0);
    EXPECT_EQ(thresholds[Smart::ATTRIBUTE_CRC_ERRORS], 0xFE);
    EXPECT_EQ(thresholds[Smart::ATTRIBUTE_TEMPERATURE], 0); // not in the page: none
}

TEST(WindowsStorageSmartMathTest, ReturnStatusSignatures)
{
    EXPECT_EQ(Smart::returnStatusFailing(0x4F, 0xC2), std::optional<bool>{false});
    EXPECT_EQ(Smart::returnStatusFailing(0xF4, 0x2C), std::optional<bool>{true});
    EXPECT_FALSE(Smart::returnStatusFailing(0x00, 0x00).has_value()); // the drive left the registers alone
    EXPECT_FALSE(Smart::returnStatusFailing(0x4F, 0x2C).has_value());
    EXPECT_FALSE(Smart::returnStatusFailing(0xF4, 0xC2).has_value());
}

TEST(WindowsStorageSmartMathTest, CommandFailedIsTheStatusRegistersErrorBit)
{
    std::array<std::uint8_t, Smart::TASK_FILE_BYTES> taskFile{};
    taskFile[Smart::TASK_FILE_COMMAND] = 0x50; // DRDY | DSC
    EXPECT_FALSE(Smart::commandFailed(taskFile));
    taskFile[Smart::TASK_FILE_COMMAND] = 0x51; // ... | ERR
    EXPECT_TRUE(Smart::commandFailed(taskFile));
    taskFile[Smart::TASK_FILE_COMMAND] = 0x00; // nothing answered at all
    EXPECT_FALSE(Smart::commandFailed(taskFile));
    EXPECT_TRUE(Smart::commandFailed(std::span(taskFile).first(7))); // too short to hold a status
}

TEST(WindowsStorageSmartMathTest, IdentifyDeviceSmartWords)
{
    Page identify{};
    identify[Smart::IDENTIFY_SMART_SUPPORTED_OFFSET] = std::byte{0x01};
    identify[Smart::IDENTIFY_SMART_ENABLED_OFFSET] = std::byte{0x01};
    Smart::IdentifySmart smart = Smart::parseIdentifySmart(identify).value_or(Smart::IdentifySmart{});
    EXPECT_TRUE(smart.supported);
    EXPECT_TRUE(smart.enabled);
    identify[Smart::IDENTIFY_SMART_ENABLED_OFFSET] = std::byte{0xFE}; // every other bit, not bit 0
    smart = Smart::parseIdentifySmart(identify).value_or(Smart::IdentifySmart{.supported = false, .enabled = true});
    EXPECT_TRUE(smart.supported);
    EXPECT_FALSE(smart.enabled);
    identify[Smart::IDENTIFY_SMART_SUPPORTED_OFFSET] = std::byte{0x00};
    identify[Smart::IDENTIFY_SMART_SUPPORTED_OFFSET + 1] = std::byte{0x01}; // the high byte isn't SMART
    EXPECT_FALSE(Smart::parseIdentifySmart(identify).value_or(Smart::IdentifySmart{.supported = true, .enabled = false}).supported);
    EXPECT_FALSE(Smart::parseIdentifySmart(std::span(identify).first(Smart::IDENTIFY_SMART_ENABLED_OFFSET + 1)).has_value());
}

TEST(WindowsStorageSmartMathTest, FailingNowIsAPreFailAttributeAtItsThreshold)
{
    const Smart::Attribute preFail{.id = 5, .flags = PREFAIL_FLAGS, .current = 10, .worst = 10, .raw = 24};
    EXPECT_TRUE(Smart::failingNow(preFail, 10));   // at the threshold
    EXPECT_TRUE(Smart::failingNow(preFail, 36));   // below it
    EXPECT_FALSE(Smart::failingNow(preFail, 9));   // above it
    EXPECT_FALSE(Smart::failingNow(preFail, 0));   // 0: always passing
    EXPECT_FALSE(Smart::failingNow(preFail, 255)); // invalid threshold
    const Smart::Attribute oldAge{.id = 9, .flags = OLD_AGE_FLAGS, .current = 10, .worst = 10, .raw = 90'000};
    EXPECT_FALSE(Smart::failingNow(oldAge, 10)); // wear, not a failure prediction
}

TEST(WindowsStorageSmartMathTest, FailingAttributesNeedThresholds)
{
    Page page = healthyPage();
    putAttribute(page, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, PREFAIL_FLAGS, 5, 5, 1'200);
    putAttribute(page, 12, Smart::ATTRIBUTE_REPORTED_UNCORRECTABLE, PREFAIL_FLAGS, 1, 1, 7);
    const std::vector<Smart::Attribute> attributes = attributesOf(page);
    Page thresholdPage{};
    putThreshold(thresholdPage, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, 10);
    putThreshold(thresholdPage, 1, Smart::ATTRIBUTE_REPORTED_UNCORRECTABLE, 0);
    EXPECT_EQ(Smart::failingAttributes(attributes, Smart::parseThresholds(thresholdPage)),
              (std::vector<std::uint8_t>{Smart::ATTRIBUTE_REALLOCATED_SECTORS}));
    EXPECT_TRUE(Smart::failingAttributes(attributes, std::nullopt).empty()); // the drive refused READ THRESHOLDS
}

TEST(WindowsStorageSmartMathTest, HealthyDriveMatchesTheLinuxRow)
{
    const std::vector<Smart::Attribute> attributes = attributesOf(healthyPage());
    Page thresholdPage{};
    putThreshold(thresholdPage, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, 10);
    const AtaHealth health = Smart::toAtaHealth(attributes, Smart::parseThresholds(thresholdPage), false);
    EXPECT_FALSE(health.failing);
    EXPECT_EQ(health.badSectors, std::optional<std::uint64_t>{0});
    EXPECT_EQ(health.powerOnHours, std::optional<std::uint64_t>{18'340});
    EXPECT_EQ(Smart::temperatureCelsius(attributes), 35);
}

TEST(WindowsStorageSmartMathTest, TheDrivesOwnVerdictOrAPreFailAttributeMakesItFailing)
{
    const std::vector<Smart::Attribute> healthy = attributesOf(healthyPage());
    EXPECT_TRUE(Smart::toAtaHealth(healthy, std::nullopt, true).failing);          // RETURN STATUS: threshold exceeded
    EXPECT_FALSE(Smart::toAtaHealth(healthy, std::nullopt, false).failing);        // RETURN STATUS: fine
    EXPECT_FALSE(Smart::toAtaHealth(healthy, std::nullopt, std::nullopt).failing); // no verdict, no thresholds: not failing

    Page page = healthyPage();
    putAttribute(page, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, PREFAIL_FLAGS, 8, 8, 1'200);
    Page thresholdPage{};
    putThreshold(thresholdPage, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, 10);
    const std::optional<Smart::Thresholds> thresholds = Smart::parseThresholds(thresholdPage);
    const AtaHealth failing = Smart::toAtaHealth(attributesOf(page), thresholds, false); // the verdict lags the attribute
    EXPECT_TRUE(failing.failing);
    EXPECT_EQ(failing.badSectors, std::optional<std::uint64_t>{1'200});
    EXPECT_FALSE(Smart::toAtaHealth(attributesOf(page), std::nullopt, false).failing); // thresholds page absent
}

TEST(WindowsStorageSmartMathTest, BadSectorsAreReallocatedPlusPending)
{
    Page page{};
    putAttribute(page, 0, Smart::ATTRIBUTE_REALLOCATED_SECTORS, PREFAIL_FLAGS, 99, 99, 24);
    putAttribute(page, 1, Smart::ATTRIBUTE_PENDING_SECTORS, OLD_AGE_FLAGS, 100, 100, 2);
    EXPECT_EQ(Smart::toAtaHealth(attributesOf(page), std::nullopt, false).badSectors, std::optional<std::uint64_t>{26});

    Page pendingOnly{};
    putAttribute(pendingOnly, 7, Smart::ATTRIBUTE_PENDING_SECTORS, OLD_AGE_FLAGS, 100, 100, 3);
    const AtaHealth pending = Smart::toAtaHealth(attributesOf(pendingOnly), std::nullopt, false);
    EXPECT_EQ(pending.badSectors, std::optional<std::uint64_t>{3});
    EXPECT_FALSE(pending.powerOnHours.has_value()); // no attribute 9

    Page neither{};
    putAttribute(neither, 0, Smart::ATTRIBUTE_CRC_ERRORS, OLD_AGE_FLAGS, 200, 200, 0);
    const AtaHealth none = Smart::toAtaHealth(attributesOf(neither), std::nullopt, false);
    EXPECT_FALSE(none.badSectors.has_value());
    EXPECT_FALSE(none.failing);
}

TEST(WindowsStorageSmartMathTest, TemperatureFromAttribute194ThenAirflow190)
{
    Page page{};
    putAttribute(page, 0, Smart::ATTRIBUTE_AIRFLOW_TEMPERATURE, OLD_AGE_FLAGS, 60, 50, 40);
    EXPECT_EQ(Smart::temperatureCelsius(attributesOf(page)), 40);                            // 190 alone
    putAttribute(page, 1, Smart::ATTRIBUTE_TEMPERATURE, OLD_AGE_FLAGS, 62, 52, 0x0100'0026); // 194 wins; its high bytes are vendor data
    EXPECT_EQ(Smart::temperatureCelsius(attributesOf(page)), 38);
    putAttribute(page, 1, Smart::ATTRIBUTE_TEMPERATURE, OLD_AGE_FLAGS, 62, 52, 0); // 194 reports nothing: 190 again
    EXPECT_EQ(Smart::temperatureCelsius(attributesOf(page)), 40);
    putAttribute(page, 1, Smart::ATTRIBUTE_TEMPERATURE, OLD_AGE_FLAGS, 62, 52, 200); // nonsense
    putAttribute(page, 0, Smart::ATTRIBUTE_AIRFLOW_TEMPERATURE, OLD_AGE_FLAGS, 60, 50, 255);
    EXPECT_FALSE(Smart::temperatureCelsius(attributesOf(page)).has_value());
    EXPECT_FALSE(Smart::temperatureCelsius({}).has_value()); // no attributes at all
}

} // namespace
} // namespace Platform
