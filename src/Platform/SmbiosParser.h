#pragma once

// A pure SMBIOS (DMTF DSP0134) table parser (#1513): the RawSMBIOSData blob Windows'
// GetSystemFirmwareTable('RSMB') returns, a bounds-checked walk of the structure table (formatted area,
// then a string set ended by a double NUL), and decoders for types 0 (BIOS), 1 (System), 2 (Baseboard)
// and 3 (Chassis). Standard library only, so it is fuzzed and unit-tested on every platform.
//
// Reuse (#1515 memory devices, #16): parseStructures() yields every structure; filter by type and read
// fields with Structure::byte()/word()/dword()/stringAt(), which return nothing past the structure's
// formatted area. Nothing here reads past the buffer it is given, whatever the input.

#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Smbios
{

using Bytes = std::span<const std::uint8_t>;

inline constexpr std::uint8_t TYPE_BIOS = 0;
inline constexpr std::uint8_t TYPE_SYSTEM = 1;
inline constexpr std::uint8_t TYPE_BASEBOARD = 2;
inline constexpr std::uint8_t TYPE_CHASSIS = 3;
inline constexpr std::uint8_t TYPE_MEMORY_DEVICE = 17;
inline constexpr std::uint8_t TYPE_END_OF_TABLE = 127;

/// A structure's header: type, length, handle.
inline constexpr std::size_t STRUCTURE_HEADER_SIZE = 4;
/// RawSMBIOSData's header: Used20CallingMethod, major, minor, DmiRevision, DWORD Length.
inline constexpr std::size_t RAW_HEADER_SIZE = 8;

/// The structure table and the SMBIOS version it follows.
struct Table
{
    std::uint8_t majorVersion = 0;
    std::uint8_t minorVersion = 0;
    Bytes data;
};

/// A string as shown: control characters made spaces, then leading and trailing spaces trimmed.
[[nodiscard]] inline std::string cleanString(Bytes bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (const std::uint8_t byte : bytes)
    {
        text.push_back(byte < 0x20 || byte == 0x7F ? ' ' : static_cast<char>(byte));
    }
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return {};
    }
    text.erase(text.find_last_not_of(' ') + 1);
    text.erase(0, first);
    return text;
}

/// One structure: its formatted area (header included, exactly its declared length) and its string set.
/// Both are views into the table it was parsed from, which must outlive it.
struct Structure
{
    std::uint8_t type = 0;
    std::uint16_t handle = 0;
    Bytes formatted; ///< The formatted area, header included
    Bytes strings;   ///< The string set: each string NUL-terminated, without the set's final NUL

    /// The byte at @p offset of the formatted area; nullopt past its end.
    [[nodiscard]] std::optional<std::uint8_t> byte(std::size_t offset) const noexcept
    {
        if (offset >= formatted.size())
        {
            return std::nullopt;
        }
        return formatted[offset];
    }

    /// A little-endian WORD at @p offset; nullopt unless all of it is inside the formatted area.
    [[nodiscard]] std::optional<std::uint16_t> word(std::size_t offset) const noexcept
    {
        if (offset >= formatted.size() || formatted.size() - offset < 2)
        {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(formatted[offset] | (formatted[offset + 1] << 8U));
    }

    /// A little-endian DWORD at @p offset; nullopt unless all of it is inside the formatted area.
    [[nodiscard]] std::optional<std::uint32_t> dword(std::size_t offset) const noexcept
    {
        if (offset >= formatted.size() || formatted.size() - offset < 4)
        {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 4; i-- > 0;)
        {
            value = (value << 8U) | formatted[offset + i];
        }
        return value;
    }

    /// String number @p index (1-based) of the string set, cleaned; empty for 0 (none) or past the end.
    [[nodiscard]] std::string string(std::uint8_t index) const
    {
        if (index == 0)
        {
            return {};
        }
        std::size_t start = 0;
        unsigned number = 1;
        for (std::size_t i = 0; i < strings.size(); ++i)
        {
            if (strings[i] != 0)
            {
                continue;
            }
            if (number == index)
            {
                return cleanString(strings.subspan(start, i - start));
            }
            ++number;
            start = i + 1;
        }
        return {};
    }

    /// The string the index byte at @p offset names; empty when that byte is past the formatted area.
    [[nodiscard]] std::string stringAt(std::size_t offset) const
    {
        const std::optional<std::uint8_t> index = byte(offset);
        return index.has_value() ? string(*index) : std::string{};
    }
};

/// The table inside a RawSMBIOSData blob; its Length is clamped to the bytes actually there. nullopt
/// when the blob is shorter than its header.
[[nodiscard]] inline std::optional<Table> parseRawSmbiosData(Bytes raw)
{
    if (raw.size() < RAW_HEADER_SIZE)
    {
        return std::nullopt;
    }
    std::uint32_t length = 0;
    for (std::size_t i = RAW_HEADER_SIZE; i-- > 4;)
    {
        length = (length << 8U) | raw[i];
    }
    const std::size_t size = std::min<std::size_t>(length, raw.size() - RAW_HEADER_SIZE);
    return Table{.majorVersion = raw[1], .minorVersion = raw[2], .data = raw.subspan(RAW_HEADER_SIZE, size)};
}

/// Every structure of a table, in order, up to and including the end-of-table structure (type 127).
/// The walk stops at the first structure that doesn't fit: a length under 4 or past the buffer, or a
/// string set without its double NUL. The structures before it are kept.
[[nodiscard]] inline std::vector<Structure> parseStructures(Bytes table)
{
    std::vector<Structure> structures;
    std::size_t offset = 0; // always <= table.size()
    while (table.size() - offset >= STRUCTURE_HEADER_SIZE)
    {
        const std::uint8_t length = table[offset + 1];
        if (length < STRUCTURE_HEADER_SIZE || length > table.size() - offset)
        {
            break;
        }
        // The string set ends at the first pair of NULs from the end of the formatted area.
        const std::size_t setStart = offset + length;
        std::size_t setEnd = setStart;
        while (setEnd + 1 < table.size() && (table[setEnd] != 0 || table[setEnd + 1] != 0))
        {
            ++setEnd;
        }
        if (setEnd + 1 >= table.size())
        {
            break; // no double NUL: truncated
        }
        Structure structure;
        structure.type = table[offset];
        structure.handle = static_cast<std::uint16_t>(table[offset + 2] | (table[offset + 3] << 8U));
        structure.formatted = table.subspan(offset, length);
        // With no strings the set is just the double NUL; otherwise keep the last string's NUL.
        structure.strings = setEnd == setStart ? Bytes{} : table.subspan(setStart, setEnd + 1 - setStart);
        structures.push_back(structure);
        if (structure.type == TYPE_END_OF_TABLE)
        {
            break;
        }
        offset = setEnd + 2;
    }
    return structures;
}

/// The first structure of @p type, or null.
[[nodiscard]] inline const Structure* findFirst(std::span<const Structure> structures, std::uint8_t type) noexcept
{
    const auto it = std::ranges::find(structures, type, &Structure::type);
    return it != structures.end() ? &*it : nullptr;
}

/// A "major.minor" release from two bytes, empty when major is 0xFF (not supported).
[[nodiscard]] inline std::string formatRelease(std::optional<std::uint8_t> major, std::optional<std::uint8_t> minor)
{
    if (!major.has_value() || !minor.has_value() || *major == 0xFF)
    {
        return {};
    }
    return std::format("{}.{}", *major, *minor);
}

/// A system UUID as text. From SMBIOS 2.6 the first three fields are little-endian (DSP0134 7.2.1).
/// Empty for all 0x00 (not present) or all 0xFF (not set), or when fewer than 16 bytes are given.
[[nodiscard]] inline std::string formatUuid(Bytes bytes, bool littleEndianFields)
{
    if (bytes.size() < 16)
    {
        return {};
    }
    bytes = bytes.first(16);
    if (std::ranges::all_of(bytes, [](std::uint8_t b) { return b == 0x00; }) ||
        std::ranges::all_of(bytes, [](std::uint8_t b) { return b == 0xFF; }))
    {
        return {};
    }
    constexpr std::array<std::size_t, 16> LITTLE{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    std::string text;
    for (std::size_t i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            text.push_back('-');
        }
        text += std::format("{:02X}", bytes[littleEndianFields ? LITTLE.at(i) : i]);
    }
    return text;
}

/// Type 0.
struct BiosFields
{
    std::string vendor;
    std::string version;
    std::string releaseDate;
    std::string embeddedControllerVersion; ///< From the EC firmware release bytes (2.4+); empty when 0xFF
};

[[nodiscard]] inline BiosFields decodeBios(const Structure& structure)
{
    return {.vendor = structure.stringAt(0x04),
            .version = structure.stringAt(0x05),
            .releaseDate = structure.stringAt(0x08),
            .embeddedControllerVersion = formatRelease(structure.byte(0x16), structure.byte(0x17))};
}

/// Type 1.
struct SystemFields
{
    std::string manufacturer;
    std::string productName;
    std::string version;
    std::string serialNumber;
    std::string uuid;
    std::string skuNumber;
    std::string family;
};

/// @param table For the version, which sets the UUID's byte order.
[[nodiscard]] inline SystemFields decodeSystem(const Structure& structure, const Table& table)
{
    constexpr std::size_t UUID_OFFSET = 0x08;
    constexpr std::size_t UUID_SIZE = 16;
    const bool littleEndian = table.majorVersion > 2 || (table.majorVersion == 2 && table.minorVersion >= 6);
    const Bytes uuid =
        structure.formatted.size() >= UUID_OFFSET + UUID_SIZE ? structure.formatted.subspan(UUID_OFFSET, UUID_SIZE) : Bytes{};
    return {.manufacturer = structure.stringAt(0x04),
            .productName = structure.stringAt(0x05),
            .version = structure.stringAt(0x06),
            .serialNumber = structure.stringAt(0x07),
            .uuid = formatUuid(uuid, littleEndian),
            .skuNumber = structure.stringAt(0x19),
            .family = structure.stringAt(0x1A)};
}

/// Type 2.
struct BaseboardFields
{
    std::string manufacturer;
    std::string product;
    std::string version;
    std::string serialNumber;
};

[[nodiscard]] inline BaseboardFields decodeBaseboard(const Structure& structure)
{
    return {.manufacturer = structure.stringAt(0x04),
            .product = structure.stringAt(0x05),
            .version = structure.stringAt(0x06),
            .serialNumber = structure.stringAt(0x07)};
}

/// Type 3.
struct ChassisFields
{
    std::string manufacturer;
    std::uint8_t type = 0; ///< The chassis type, lock bit cleared; 0 when absent
    bool hasLock = false;  ///< Bit 7 of the type byte
    std::string version;
    std::string serialNumber;
};

inline constexpr std::uint8_t CHASSIS_LOCK_BIT = 0x80;
inline constexpr std::uint8_t CHASSIS_TYPE_MASK = 0x7F;

[[nodiscard]] inline ChassisFields decodeChassis(const Structure& structure)
{
    const std::uint8_t raw = structure.byte(0x05).value_or(0);
    return {.manufacturer = structure.stringAt(0x04),
            .type = static_cast<std::uint8_t>(raw & CHASSIS_TYPE_MASK),
            .hasLock = (raw & CHASSIS_LOCK_BIT) != 0,
            .version = structure.stringAt(0x06),
            .serialNumber = structure.stringAt(0x07)};
}

/// The name of a chassis type (DSP0134 7.4.1); the lock bit is ignored. Empty for 0 or an unknown value.
[[nodiscard]] constexpr std::string_view chassisTypeName(std::uint8_t type)
{
    constexpr std::array<std::string_view, 0x25> NAMES{
        "",
        "Other",
        "Unknown",
        "Desktop",
        "Low Profile Desktop",
        "Pizza Box",
        "Mini Tower",
        "Tower",
        "Portable",
        "Laptop",
        "Notebook",
        "Hand Held",
        "Docking Station",
        "All in One",
        "Sub Notebook",
        "Space-saving",
        "Lunch Box",
        "Main Server Chassis",
        "Expansion Chassis",
        "SubChassis",
        "Bus Expansion Chassis",
        "Peripheral Chassis",
        "RAID Chassis",
        "Rack Mount Chassis",
        "Sealed-case PC",
        "Multi-system Chassis",
        "Compact PCI",
        "Advanced TCA",
        "Blade",
        "Blade Enclosure",
        "Tablet",
        "Convertible",
        "Detachable",
        "IoT Gateway",
        "Embedded PC",
        "Mini PC",
        "Stick PC",
    };
    const auto index = static_cast<std::size_t>(type & CHASSIS_TYPE_MASK);
    return index < NAMES.size() ? NAMES.at(index) : std::string_view{};
}

/// The platform role a chassis type implies: "Desktop", "Mobile", "Server" or "Docking station"; empty
/// for the rest. Linux's role, and Windows' when the power manager doesn't say.
[[nodiscard]] constexpr std::string_view platformRoleFromChassis(std::uint8_t type) noexcept
{
    switch (type & CHASSIS_TYPE_MASK)
    {
    case 0x03: // Desktop
    case 0x04: // Low Profile Desktop
    case 0x05: // Pizza Box
    case 0x06: // Mini Tower
    case 0x07: // Tower
    case 0x0D: // All in One
    case 0x0F: // Space-saving
    case 0x10: // Lunch Box
    case 0x18: // Sealed-case PC
    case 0x22: // Embedded PC
    case 0x23: // Mini PC
    case 0x24: // Stick PC
        return "Desktop";
    case 0x08: // Portable
    case 0x09: // Laptop
    case 0x0A: // Notebook
    case 0x0B: // Hand Held
    case 0x0E: // Sub Notebook
    case 0x1E: // Tablet
    case 0x1F: // Convertible
    case 0x20: // Detachable
        return "Mobile";
    case 0x11: // Main Server Chassis
    case 0x17: // Rack Mount Chassis
    case 0x19: // Multi-system Chassis
    case 0x1C: // Blade
    case 0x1D: // Blade Enclosure
        return "Server";
    default:
        return {};
    }
}

/// "major.minor" from an SMBIOS entry point (/sys/firmware/dmi/tables/smbios_entry_point): the 64-bit
/// "_SM3_" one or the 32-bit "_SM_" one. Empty for anything else.
[[nodiscard]] inline std::string entryPointVersion(Bytes entryPoint)
{
    const auto startsWith = [entryPoint](std::string_view anchor)
    {
        return entryPoint.size() >= anchor.size() &&
               std::ranges::equal(
                   entryPoint.first(anchor.size()), anchor, [](std::uint8_t b, char c) { return b == static_cast<std::uint8_t>(c); });
    };
    constexpr std::size_t SM3_SIZE = 0x18;
    constexpr std::size_t SM_SIZE = 0x1F;
    if (startsWith("_SM3_") && entryPoint.size() >= SM3_SIZE)
    {
        return std::format("{}.{}", entryPoint[7], entryPoint[8]);
    }
    if (startsWith("_SM_") && entryPoint.size() >= SM_SIZE)
    {
        return std::format("{}.{}", entryPoint[6], entryPoint[7]);
    }
    return {};
}

/// Fills @p info's firmware, system, board and chassis facts from a RawSMBIOSData blob. Leaves them
/// empty (and returns false) when the blob is too short to hold a table.
inline bool decodeFirmware(Bytes rawSmbiosData, FirmwareInfo& info)
{
    const std::optional<Table> table = parseRawSmbiosData(rawSmbiosData);
    if (!table.has_value())
    {
        return false;
    }
    info.smbiosVersion = std::format("{}.{}", table->majorVersion, table->minorVersion);
    const std::vector<Structure> structures = parseStructures(table->data);
    if (const Structure* bios = findFirst(structures, TYPE_BIOS); bios != nullptr)
    {
        BiosFields fields = decodeBios(*bios);
        info.biosVendor = std::move(fields.vendor);
        info.biosVersion = std::move(fields.version);
        info.biosReleaseDate = std::move(fields.releaseDate);
        info.embeddedControllerVersion = std::move(fields.embeddedControllerVersion);
    }
    if (const Structure* system = findFirst(structures, TYPE_SYSTEM); system != nullptr)
    {
        SystemFields fields = decodeSystem(*system, *table);
        info.systemManufacturer = std::move(fields.manufacturer);
        info.systemModel = std::move(fields.productName);
        info.systemVersion = std::move(fields.version);
        info.systemSerial = std::move(fields.serialNumber);
        info.systemUuid = std::move(fields.uuid);
        info.systemSku = std::move(fields.skuNumber);
        info.systemFamily = std::move(fields.family);
    }
    if (const Structure* board = findFirst(structures, TYPE_BASEBOARD); board != nullptr)
    {
        BaseboardFields fields = decodeBaseboard(*board);
        info.boardManufacturer = std::move(fields.manufacturer);
        info.boardProduct = std::move(fields.product);
        info.boardVersion = std::move(fields.version);
        info.boardSerial = std::move(fields.serialNumber);
    }
    if (const Structure* chassis = findFirst(structures, TYPE_CHASSIS); chassis != nullptr)
    {
        ChassisFields fields = decodeChassis(*chassis);
        info.chassisManufacturer = std::move(fields.manufacturer);
        info.chassisType = std::string(chassisTypeName(fields.type));
        info.platformRole = std::string(platformRoleFromChassis(fields.type));
    }
    return true;
}

} // namespace Platform::Smbios
