#pragma once

// Partition layouts (#1632), shared by both platforms and standard library only, so the tests and the
// fuzz target (tests/fuzz/fuzz_partition_layout.cpp) run everywhere:
// - the common GPT partition type GUIDs and MBR partition type bytes, by name;
// - Windows' DRIVE_LAYOUT_INFORMATION_EX (IOCTL_DISK_GET_DRIVE_LAYOUT_EX) parsed from its bytes at the
//   documented offsets, which WindowsStorage's tests check against <winioctl.h>'s own structs.

#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::PartitionTable
{

/// A GUID's 16 bytes (Windows' GUID struct: Data1, Data2 and Data3 little endian, Data4 in order) as
/// "C12A7328-F81F-11D2-BA4B-00A0C93EC93B".
[[nodiscard]] inline std::string formatGuid(std::span<const std::byte, 16> bytes)
{
    const auto at = [bytes](std::size_t index)
    {
        return static_cast<unsigned>(bytes[index]);
    };
    return std::format("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                       at(3),
                       at(2),
                       at(1),
                       at(0),
                       at(5),
                       at(4),
                       at(7),
                       at(6),
                       at(8),
                       at(9),
                       at(10),
                       at(11),
                       at(12),
                       at(13),
                       at(14),
                       at(15));
}

/// @p guid upper-cased (udev reports type GUIDs in lower case).
[[nodiscard]] inline std::string upperGuid(std::string_view guid)
{
    std::string text(guid);
    std::ranges::transform(text, text.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return text;
}

/// A GPT partition type GUID's common name; empty for one not in the table. Case-insensitive.
[[nodiscard]] inline std::string_view gptTypeName(std::string_view guid)
{
    constexpr std::array<std::pair<std::string_view, std::string_view>, 20> NAMES{{
        {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System"},          {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved"},
        {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Basic data"},          {"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Recovery"},
        {"5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", "LDM metadata"},        {"AF9B60A0-1431-4F62-BC68-3311714A69AD", "LDM data"},
        {"E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D", "Storage Spaces"},      {"21686148-6449-6E6F-744E-656564454649", "BIOS boot"},
        {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem"},    {"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap"},
        {"E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM"},           {"A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID"},
        {"CA7D7CCB-63ED-4C53-861C-1742536059CC", "Linux LUKS"},          {"4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)"},
        {"B921B045-1DF0-41C3-AF44-4C6F280D3FAE", "Linux root (ARM64)"},  {"933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux home"},
        {"BC13C2FF-59E6-4262-A352-B275FD6F7172", "Linux extended boot"}, {"6A898CC3-1DD2-11B2-99A6-080020736631", "ZFS"},
        {"48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+"},          {"7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS"},
    }};
    const std::string upper = upperGuid(guid);
    const auto found = std::ranges::find(NAMES, std::string_view(upper), &std::pair<std::string_view, std::string_view>::first);
    return found == NAMES.end() ? std::string_view{} : found->second;
}

/// An MBR partition type byte's common name; empty for one not in the table (and for 0, an unused slot).
[[nodiscard]] inline std::string_view mbrTypeName(std::uint8_t type)
{
    switch (type)
    {
    case 0x01:
        return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT16";
    case 0x05:
    case 0x0F:
        return "Extended";
    case 0x07:
        return "NTFS/exFAT";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x27:
        return "Recovery";
    case 0x42:
        return "LDM";
    case 0x82:
        return "Linux swap";
    case 0x83:
        return "Linux filesystem";
    case 0x85:
        return "Linux extended";
    case 0x8E:
        return "Linux LVM";
    case 0xA5:
        return "FreeBSD";
    case 0xAF:
        return "Apple HFS+";
    case 0xEE:
        return "GPT protective";
    case 0xEF:
        return "EFI System";
    case 0xFD:
        return "Linux RAID";
    default:
        return {};
    }
}

/// An MBR type as typeId shows it: "0x07".
[[nodiscard]] inline std::string mbrTypeId(std::uint8_t type)
{
    return std::format("0x{:02X}", static_cast<unsigned>(type));
}

/// udev's MBR ID_PART_ENTRY_TYPE ("0x83", "0x7"): the byte, or nullopt when it isn't one.
[[nodiscard]] inline std::optional<std::uint8_t> parseMbrType(std::string_view text)
{
    if (!text.starts_with("0x") && !text.starts_with("0X"))
    {
        return std::nullopt;
    }
    text.remove_prefix(2);
    if (text.empty() || text.size() > 2)
    {
        return std::nullopt;
    }
    unsigned value = 0;
    for (const char c : text)
    {
        const int digit = std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0'
                        : (c >= 'a' && c <= 'f')                           ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F')                           ? c - 'A' + 10
                                                                           : -1;
        if (digit < 0)
        {
            return std::nullopt;
        }
        value = (value * 16U) + static_cast<unsigned>(digit);
    }
    return static_cast<std::uint8_t>(value);
}

/// Fills @p partition's typeId and typeName from a GPT type GUID or an MBR type ("0x83"), as udev
/// reports them; both stay empty for an empty @p type.
inline void setType(Partition& partition, std::string_view type)
{
    if (const std::optional<std::uint8_t> mbr = parseMbrType(type); mbr.has_value())
    {
        partition.typeId = mbrTypeId(*mbr);
        partition.typeName = std::string(mbrTypeName(*mbr));
        return;
    }
    partition.typeId = upperGuid(type);
    partition.typeName = std::string(gptTypeName(type));
}

// DRIVE_LAYOUT_INFORMATION_EX, as <winioctl.h> lays it out (the same on x86, x64 and ARM64):
//   0 PartitionStyle (DWORD), 4 PartitionCount (DWORD), 8 the MBR/GPT union (40 bytes),
//   48 PartitionEntry[PartitionCount], PARTITION_INFORMATION_EX of 144 bytes each:
//     0 PartitionStyle, 8 StartingOffset (LONGLONG), 16 PartitionLength (LONGLONG), 24 PartitionNumber
//     (DWORD), 32 the union: MBR PartitionType (BYTE) / GPT PartitionType (GUID).
inline constexpr std::size_t LAYOUT_HEADER_BYTES = 48;
inline constexpr std::size_t LAYOUT_ENTRY_BYTES = 144;
inline constexpr std::size_t ENTRY_STARTING_OFFSET = 8;
inline constexpr std::size_t ENTRY_LENGTH = 16;
inline constexpr std::size_t ENTRY_NUMBER = 24;
inline constexpr std::size_t ENTRY_TYPE = 32;

/// PARTITION_STYLE's values.
inline constexpr std::uint32_t STYLE_MBR = 0;
inline constexpr std::uint32_t STYLE_GPT = 1;
inline constexpr std::uint32_t STYLE_RAW = 2;

/// A drive's style and partitions.
struct DriveLayout
{
    PartitionStyle style = PartitionStyle::Unknown;
    std::vector<Partition> partitions;
};

/// The little-endian unsigned integer of @p size bytes at @p offset; the caller checks the bounds.
[[nodiscard]] inline std::uint64_t readLittleEndian(std::span<const std::byte> bytes, std::size_t offset, std::size_t size)
{
    std::uint64_t value = 0;
    std::size_t i = size;
    while (i > 0)
    {
        --i;
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

/// A DRIVE_LAYOUT_INFORMATION_EX buffer's style and partitions. Unused MBR slots (type 0), unnumbered
/// entries (an extended partition's container) and empty ones are left out; entries the buffer is too short for are too, whatever
/// PartitionCount says. nullopt when the buffer is too short for the header.
[[nodiscard]] inline std::optional<DriveLayout> parseDriveLayout(std::span<const std::byte> buffer)
{
    if (buffer.size() < LAYOUT_HEADER_BYTES)
    {
        return std::nullopt;
    }
    DriveLayout layout;
    const auto style = static_cast<std::uint32_t>(readLittleEndian(buffer, 0, 4));
    switch (style)
    {
    case STYLE_MBR:
        layout.style = PartitionStyle::Mbr;
        break;
    case STYLE_GPT:
        layout.style = PartitionStyle::Gpt;
        break;
    case STYLE_RAW:
        layout.style = PartitionStyle::Raw;
        return layout;
    default:
        return layout; // a style this parser doesn't know: no entries it can read
    }
    const std::uint64_t count = readLittleEndian(buffer, 4, 4);
    const std::size_t fit = (buffer.size() - LAYOUT_HEADER_BYTES) / LAYOUT_ENTRY_BYTES;
    const std::size_t entries = static_cast<std::size_t>(std::min<std::uint64_t>(count, fit));
    std::size_t index = 0;
    while (index < entries)
    {
        const std::span<const std::byte> entry = buffer.subspan(LAYOUT_HEADER_BYTES + (index * LAYOUT_ENTRY_BYTES), LAYOUT_ENTRY_BYTES);
        ++index;
        const auto offset = static_cast<std::int64_t>(readLittleEndian(entry, ENTRY_STARTING_OFFSET, 8));
        const auto length = static_cast<std::int64_t>(readLittleEndian(entry, ENTRY_LENGTH, 8));
        if (offset < 0 || length <= 0)
        {
            continue;
        }
        Partition partition;
        partition.number = static_cast<std::uint32_t>(readLittleEndian(entry, ENTRY_NUMBER, 4));
        if (partition.number == 0)
        {
            continue; // an MBR extended partition's container: Windows numbers only the logical drives in it
        }
        partition.offsetBytes = static_cast<std::uint64_t>(offset);
        partition.sizeBytes = static_cast<std::uint64_t>(length);
        if (layout.style == PartitionStyle::Mbr)
        {
            const auto type = static_cast<std::uint8_t>(entry[ENTRY_TYPE]);
            if (type == 0)
            {
                continue; // PARTITION_ENTRY_UNUSED
            }
            partition.typeId = mbrTypeId(type);
            partition.typeName = std::string(mbrTypeName(type));
        }
        else
        {
            partition.typeId = formatGuid(entry.subspan<ENTRY_TYPE, 16>());
            partition.typeName = std::string(gptTypeName(partition.typeId));
        }
        layout.partitions.push_back(std::move(partition));
    }
    return layout;
}

} // namespace Platform::PartitionTable
