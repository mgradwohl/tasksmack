#pragma once

/// @file PartitionLayoutTestData.h
/// @brief Hand-built DRIVE_LAYOUT_INFORMATION_EX buffers (#1632) for the PartitionTable parser tests and
/// the Windows storage function-table fakes.

#include "Platform/PartitionTable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Platform::PartitionTable::TestData
{

inline constexpr std::uint64_t MIB = std::uint64_t{1024} * 1024;
inline constexpr std::uint64_t GIB = MIB * 1024;

inline constexpr std::string_view EFI_SYSTEM = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
inline constexpr std::string_view MICROSOFT_RESERVED = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
inline constexpr std::string_view BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
inline constexpr std::string_view RECOVERY = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC";

/// "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" as a GUID struct's 16 bytes (the first three groups little endian).
[[nodiscard]] inline std::array<std::byte, 16> guidBytes(std::string_view text)
{
    std::array<std::uint8_t, 16> big{};
    std::size_t out = 0;
    std::size_t i = 0;
    const auto nibble = [](char c) -> unsigned
    {
        if (c <= '9')
        {
            return static_cast<unsigned>(c - '0');
        }
        return static_cast<unsigned>((c >= 'a' ? c - 'a' : c - 'A') + 10);
    };
    while (i + 1 < text.size() && out < big.size())
    {
        if (text[i] == '-')
        {
            ++i;
            continue;
        }
        big.at(out) = static_cast<std::uint8_t>((nibble(text[i]) << 4U) | nibble(text[i + 1]));
        ++out;
        i += 2;
    }
    constexpr std::array<std::size_t, 16> ORDER{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    std::array<std::byte, 16> bytes{};
    std::size_t index = 0;
    while (index < bytes.size())
    {
        bytes.at(index) = static_cast<std::byte>(big.at(ORDER.at(index)));
        ++index;
    }
    return bytes;
}

/// One PARTITION_INFORMATION_EX to lay out.
struct Entry
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t number = 0;
    std::uint8_t mbrType = 0; ///< An MBR layout's type byte
    std::string_view gptType; ///< A GPT layout's type GUID
};

/// Writes @p value's low @p size bytes little endian at @p offset.
inline void putLittleEndian(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value, std::size_t size)
{
    std::size_t i = 0;
    while (i < size)
    {
        bytes.at(offset + i) = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
        ++i;
    }
}

/// A DRIVE_LAYOUT_INFORMATION_EX of @p style (STYLE_MBR, STYLE_GPT, STYLE_RAW) holding @p entries.
[[nodiscard]] inline std::vector<std::byte> driveLayout(std::uint32_t style, const std::vector<Entry>& entries)
{
    std::vector<std::byte> bytes(LAYOUT_HEADER_BYTES + (entries.size() * LAYOUT_ENTRY_BYTES));
    putLittleEndian(bytes, 0, style, 4);
    putLittleEndian(bytes, 4, entries.size(), 4);
    std::size_t index = 0;
    while (index < entries.size())
    {
        const Entry& entry = entries[index];
        const std::size_t at = LAYOUT_HEADER_BYTES + (index * LAYOUT_ENTRY_BYTES);
        putLittleEndian(bytes, at, style, 4);
        putLittleEndian(bytes, at + ENTRY_STARTING_OFFSET, entry.offset, 8);
        putLittleEndian(bytes, at + ENTRY_LENGTH, entry.length, 8);
        putLittleEndian(bytes, at + ENTRY_NUMBER, entry.number, 4);
        if (style == STYLE_MBR)
        {
            bytes.at(at + ENTRY_TYPE) = static_cast<std::byte>(entry.mbrType);
        }
        else if (!entry.gptType.empty())
        {
            const std::array<std::byte, 16> guid = guidBytes(entry.gptType);
            std::size_t i = 0;
            while (i < guid.size())
            {
                bytes.at(at + ENTRY_TYPE + i) = guid.at(i);
                ++i;
            }
        }
        ++index;
    }
    return bytes;
}

/// A Windows 11 install disk: EFI System, Microsoft reserved, Basic data (C:), Recovery.
[[nodiscard]] inline std::vector<std::byte> windowsGptLayout()
{
    return driveLayout(STYLE_GPT,
                       {
                           {.offset = MIB, .length = 260 * MIB, .number = 1, .mbrType = 0, .gptType = EFI_SYSTEM},
                           {.offset = 261 * MIB, .length = 16 * MIB, .number = 2, .mbrType = 0, .gptType = MICROSOFT_RESERVED},
                           {.offset = 277 * MIB, .length = 930 * GIB, .number = 3, .mbrType = 0, .gptType = BASIC_DATA},
                           {.offset = (277 * MIB) + (930 * GIB), .length = 750 * MIB, .number = 4, .mbrType = 0, .gptType = RECOVERY},
                       });
}

/// An MBR disk as Windows reports it: four slots, two used (NTFS and Linux), one unused, one an extended
/// partition's container (number 0).
[[nodiscard]] inline std::vector<std::byte> mbrLayout()
{
    return driveLayout(STYLE_MBR,
                       {
                           {.offset = MIB, .length = 100 * GIB, .number = 1, .mbrType = 0x07, .gptType = {}},
                           {.offset = 0, .length = 0, .number = 0, .mbrType = 0x00, .gptType = {}}, // unused slot
                           {.offset = 101 * GIB, .length = 10 * GIB, .number = 0, .mbrType = 0x0F, .gptType = {}},
                           {.offset = 111 * GIB, .length = 8 * GIB, .number = 2, .mbrType = 0x83, .gptType = {}},
                       });
}

} // namespace Platform::PartitionTable::TestData
