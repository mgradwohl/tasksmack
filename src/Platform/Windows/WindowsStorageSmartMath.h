#pragma once

// The pure parts of an ATA drive's SMART health on Windows (#2588): the bytes SMART READ DATA, SMART READ
// THRESHOLDS, SMART RETURN STATUS and IDENTIFY DEVICE answer with, parsed into the AtaHealth that System
// Information › Storage shows -- the same facts Linux gets from udisks2 (#1631): the drive's own verdict,
// its bad sectors and its power-on hours, plus the temperature where Windows' temperature property gave
// none. Plain values, so it reaches no Windows header and its tests build and run on every platform
// (#1133); the ATA pass-through that fetches the pages is WindowsStorage.h's.
//
// The layouts (ATA/ATAPI Command Set and the SFF-8035i SMART specification):
// - SMART READ DATA (feature D0h) answers 512 bytes: a version word, then 30 attribute entries of 12 bytes
//   from byte 2: id (0 is an empty slot), flags (little-endian word; bit 0 is "pre-fail"), the current
//   normalized value, the worst value, six bytes of raw value (little endian), one vendor byte.
// - SMART READ THRESHOLDS (D1h) answers 512 bytes with the same table shape: id, then the threshold.
// - SMART RETURN STATUS (DAh) answers in the task file: LBA mid/high 4Fh/C2h for "no threshold exceeded",
//   F4h/2Ch for "threshold exceeded" -- the drive's own assessment, what udisks2's SmartFailing reports.
// - IDENTIFY DEVICE (ECh) answers 512 bytes; word 82 bit 0 says SMART is supported, word 85 bit 0 that
//   it is enabled.

#include "Platform/ISystemInfoProbe.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Platform::AtaSmartMath
{

/// The ATA commands and the SMART feature codes, as plain bytes.
inline constexpr std::uint8_t IDENTIFY_DEVICE_COMMAND = 0xEC;
inline constexpr std::uint8_t SMART_COMMAND = 0xB0;
inline constexpr std::uint8_t SMART_READ_DATA = 0xD0;
inline constexpr std::uint8_t SMART_READ_THRESHOLDS = 0xD1;
inline constexpr std::uint8_t SMART_RETURN_STATUS = 0xDA;
/// Every SMART command carries this signature in LBA mid and LBA high.
inline constexpr std::uint8_t SMART_LBA_MID = 0x4F;
inline constexpr std::uint8_t SMART_LBA_HIGH = 0xC2;
/// SMART RETURN STATUS answers with these in LBA mid and LBA high when a threshold is exceeded.
inline constexpr std::uint8_t SMART_FAILING_LBA_MID = 0xF4;
inline constexpr std::uint8_t SMART_FAILING_LBA_HIGH = 0x2C;

/// A command's data page and the attribute table inside SMART READ DATA / READ THRESHOLDS.
inline constexpr std::size_t PAGE_BYTES = 512;
inline constexpr std::size_t ATTRIBUTE_TABLE_OFFSET = 2;
inline constexpr std::size_t ATTRIBUTE_COUNT = 30;
inline constexpr std::size_t ATTRIBUTE_BYTES = 12;
inline constexpr std::size_t ATTRIBUTE_TABLE_BYTES = ATTRIBUTE_COUNT * ATTRIBUTE_BYTES;
inline constexpr std::size_t RAW_VALUE_BYTES = 6;
/// Attribute flags bit 0: the attribute predicts failure (pre-fail), rather than counting wear (old age).
inline constexpr std::uint16_t FLAG_PREFAIL = 0x0001;

/// The task file an ATA pass-through sends and receives: 8 bytes, these their indices. On the way in
/// index 6 holds the command; on the way back, the status register, whose bit 0 says the command failed.
inline constexpr std::size_t TASK_FILE_BYTES = 8;
inline constexpr std::size_t TASK_FILE_FEATURES = 0; ///< The error register on the way back
inline constexpr std::size_t TASK_FILE_COUNT = 1;
inline constexpr std::size_t TASK_FILE_LBA_LOW = 2;
inline constexpr std::size_t TASK_FILE_LBA_MID = 3;
inline constexpr std::size_t TASK_FILE_LBA_HIGH = 4;
inline constexpr std::size_t TASK_FILE_DEVICE = 5;
inline constexpr std::size_t TASK_FILE_COMMAND = 6; ///< The status register on the way back
inline constexpr std::uint8_t STATUS_ERROR = 0x01;
/// The device register's obsolete "set" bits (7 and 5) with drive 0 selected, as the SMART commands of
/// older specifications carry it; a SATA port has one drive and ignores the selection.
inline constexpr std::uint8_t DEVICE_REGISTER = 0xA0;

/// The attribute ids the health row is built from (the common vendor assignments).
inline constexpr std::uint8_t ATTRIBUTE_REALLOCATED_SECTORS = 5;
inline constexpr std::uint8_t ATTRIBUTE_POWER_ON_HOURS = 9;
inline constexpr std::uint8_t ATTRIBUTE_REPORTED_UNCORRECTABLE = 187;
inline constexpr std::uint8_t ATTRIBUTE_COMMAND_TIMEOUT = 188;
inline constexpr std::uint8_t ATTRIBUTE_AIRFLOW_TEMPERATURE = 190;
inline constexpr std::uint8_t ATTRIBUTE_TEMPERATURE = 194;
inline constexpr std::uint8_t ATTRIBUTE_REALLOCATED_EVENTS = 196;
inline constexpr std::uint8_t ATTRIBUTE_PENDING_SECTORS = 197;
inline constexpr std::uint8_t ATTRIBUTE_OFFLINE_UNCORRECTABLE = 198;
inline constexpr std::uint8_t ATTRIBUTE_CRC_ERRORS = 199;

/// IDENTIFY DEVICE's SMART words: word 82 (command sets supported) and word 85 (enabled), bit 0 each.
inline constexpr std::size_t IDENTIFY_SMART_SUPPORTED_OFFSET = std::size_t{82} * 2;
inline constexpr std::size_t IDENTIFY_SMART_ENABLED_OFFSET = std::size_t{85} * 2;
inline constexpr std::uint16_t IDENTIFY_SMART_BIT = 0x0001;

/// One entry of the SMART attribute table.
struct Attribute
{
    std::uint8_t id = 0;
    std::uint16_t flags = 0;
    std::uint8_t current = 0; ///< The normalized value, 1 (worst) to 253 (best) by convention
    std::uint8_t worst = 0;   ///< The lowest normalized value the drive has seen
    std::uint64_t raw = 0;    ///< The 48-bit raw value, little endian

    [[nodiscard]] constexpr bool preFail() const noexcept
    {
        return (flags & FLAG_PREFAIL) != 0;
    }
};

/// Each attribute id's threshold, indexed by id; 0 means none (a threshold of 0 is "always passing").
using Thresholds = std::array<std::uint8_t, 256>;

/// The little-endian unsigned value of @p bytes (up to 8).
[[nodiscard]] constexpr std::uint64_t littleEndian(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t value = 0;
    std::size_t index = 0;
    while (index < bytes.size() && index < sizeof(std::uint64_t))
    {
        value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
        ++index;
    }
    return value;
}

/// The attributes in a SMART READ DATA page, in table order, skipping empty slots (id 0). nullopt when the
/// page is shorter than the table.
[[nodiscard]] inline std::optional<std::vector<Attribute>> parseAttributes(std::span<const std::byte> page)
{
    if (page.size() < ATTRIBUTE_TABLE_OFFSET + ATTRIBUTE_TABLE_BYTES)
    {
        return std::nullopt;
    }
    std::vector<Attribute> attributes;
    std::size_t slot = 0;
    while (slot < ATTRIBUTE_COUNT)
    {
        const std::span<const std::byte> entry = page.subspan(ATTRIBUTE_TABLE_OFFSET + (slot * ATTRIBUTE_BYTES), ATTRIBUTE_BYTES);
        ++slot;
        const auto id = static_cast<std::uint8_t>(entry[0]);
        if (id == 0)
        {
            continue;
        }
        attributes.push_back(Attribute{.id = id,
                                       .flags = static_cast<std::uint16_t>(littleEndian(entry.subspan(1, 2))),
                                       .current = static_cast<std::uint8_t>(entry[3]),
                                       .worst = static_cast<std::uint8_t>(entry[4]),
                                       .raw = littleEndian(entry.subspan(5, RAW_VALUE_BYTES))});
    }
    return attributes;
}

/// The thresholds in a SMART READ THRESHOLDS page, by attribute id. nullopt when the page is shorter than
/// the table.
[[nodiscard]] inline std::optional<Thresholds> parseThresholds(std::span<const std::byte> page)
{
    if (page.size() < ATTRIBUTE_TABLE_OFFSET + ATTRIBUTE_TABLE_BYTES)
    {
        return std::nullopt;
    }
    Thresholds thresholds{};
    std::size_t slot = 0;
    while (slot < ATTRIBUTE_COUNT)
    {
        const std::size_t at = ATTRIBUTE_TABLE_OFFSET + (slot * ATTRIBUTE_BYTES);
        ++slot;
        const auto id = static_cast<std::uint8_t>(page[at]);
        if (id != 0)
        {
            thresholds.at(id) = static_cast<std::uint8_t>(page[at + 1]);
        }
    }
    return thresholds;
}

/// Whether the command whose task file came back as @p taskFile failed (status register bit 0, ERR).
[[nodiscard]] constexpr bool commandFailed(std::span<const std::uint8_t> taskFile) noexcept
{
    return taskFile.size() < TASK_FILE_BYTES || (taskFile[TASK_FILE_COMMAND] & STATUS_ERROR) != 0;
}

/// SMART RETURN STATUS's verdict from the LBA mid and high bytes it answered with: false for the
/// "no threshold exceeded" signature, true for "threshold exceeded", nullopt for anything else (the
/// drive didn't answer the command the way the specification says).
[[nodiscard]] constexpr std::optional<bool> returnStatusFailing(std::uint8_t lbaMid, std::uint8_t lbaHigh) noexcept
{
    if (lbaMid == SMART_LBA_MID && lbaHigh == SMART_LBA_HIGH)
    {
        return false;
    }
    if (lbaMid == SMART_FAILING_LBA_MID && lbaHigh == SMART_FAILING_LBA_HIGH)
    {
        return true;
    }
    return std::nullopt;
}

/// What IDENTIFY DEVICE says about SMART.
struct IdentifySmart
{
    bool supported = false;
    bool enabled = false;
};

/// The SMART words of an IDENTIFY DEVICE page; nullopt when the page is short.
[[nodiscard]] inline std::optional<IdentifySmart> parseIdentifySmart(std::span<const std::byte> identify)
{
    if (identify.size() < IDENTIFY_SMART_ENABLED_OFFSET + 2)
    {
        return std::nullopt;
    }
    return IdentifySmart{.supported = (littleEndian(identify.subspan(IDENTIFY_SMART_SUPPORTED_OFFSET, 2)) & IDENTIFY_SMART_BIT) != 0,
                         .enabled = (littleEndian(identify.subspan(IDENTIFY_SMART_ENABLED_OFFSET, 2)) & IDENTIFY_SMART_BIT) != 0};
}

/// The attribute with @p id in @p attributes, or nullptr.
[[nodiscard]] inline const Attribute* findAttribute(std::span<const Attribute> attributes, std::uint8_t id) noexcept
{
    for (const Attribute& attribute : attributes)
    {
        if (attribute.id == id)
        {
            return &attribute;
        }
    }
    return nullptr;
}

/// Whether a pre-fail attribute is failing now: its current value has reached its threshold. A threshold
/// of 0 is "always passing" and 0xFF is invalid, so neither fails anything; an old-age attribute at its
/// threshold is wear, not a failure prediction.
[[nodiscard]] constexpr bool failingNow(const Attribute& attribute, std::uint8_t threshold) noexcept
{
    constexpr std::uint8_t INVALID_THRESHOLD = 0xFF;
    return attribute.preFail() && threshold != 0 && threshold != INVALID_THRESHOLD && attribute.current <= threshold;
}

/// The ids of the pre-fail attributes failing now, in table order; empty without thresholds.
[[nodiscard]] inline std::vector<std::uint8_t> failingAttributes(std::span<const Attribute> attributes,
                                                                 const std::optional<Thresholds>& thresholds)
{
    std::vector<std::uint8_t> failing;
    if (!thresholds.has_value())
    {
        return failing;
    }
    for (const Attribute& attribute : attributes)
    {
        if (failingNow(attribute, thresholds->at(attribute.id)))
        {
            failing.push_back(attribute.id);
        }
    }
    return failing;
}

/// The drive's temperature in whole degrees Celsius from attribute 194 (Temperature), else 190 (Airflow
/// temperature): the low byte of the raw value, as drives report it (the higher bytes hold vendor minimums
/// and maximums). An implausible reading (0, or above 127) is skipped for the next attribute; nullopt when
/// neither gives one.
[[nodiscard]] inline std::optional<int> temperatureCelsius(std::span<const Attribute> attributes) noexcept
{
    constexpr std::uint64_t LOW_BYTE = 0xFF;
    constexpr std::uint64_t MAX_PLAUSIBLE = 127;
    constexpr std::array<std::uint8_t, 2> IDS{ATTRIBUTE_TEMPERATURE, ATTRIBUTE_AIRFLOW_TEMPERATURE};
    for (const std::uint8_t id : IDS)
    {
        if (const Attribute* attribute = findAttribute(attributes, id); attribute != nullptr)
        {
            const std::uint64_t celsius = attribute->raw & LOW_BYTE;
            if (celsius != 0 && celsius <= MAX_PLAUSIBLE)
            {
                return static_cast<int>(celsius);
            }
        }
    }
    return std::nullopt;
}

/// The health row's facts from the attributes, their thresholds (when the drive answered READ
/// THRESHOLDS) and SMART RETURN STATUS's verdict (when it answered that): the drive is failing when its
/// own verdict says so or a pre-fail attribute has reached its threshold; the bad sectors are the
/// reallocated (5) plus the pending (197) raw counts, as udisks2 counts them, when the drive reports
/// either; the power-on hours attribute 9's raw value, when the drive has it.
[[nodiscard]] inline AtaHealth
toAtaHealth(std::span<const Attribute> attributes, const std::optional<Thresholds>& thresholds, std::optional<bool> statusFailing)
{
    AtaHealth health;
    health.failing = statusFailing.value_or(false) || !failingAttributes(attributes, thresholds).empty();
    const Attribute* reallocated = findAttribute(attributes, ATTRIBUTE_REALLOCATED_SECTORS);
    const Attribute* pending = findAttribute(attributes, ATTRIBUTE_PENDING_SECTORS);
    if (reallocated != nullptr || pending != nullptr)
    {
        health.badSectors = (reallocated != nullptr ? reallocated->raw : 0) + (pending != nullptr ? pending->raw : 0);
    }
    if (const Attribute* hours = findAttribute(attributes, ATTRIBUTE_POWER_ON_HOURS); hours != nullptr)
    {
        health.powerOnHours = hours->raw;
    }
    return health;
}

} // namespace Platform::AtaSmartMath
