#pragma once

// A monitor's EDID base block (VESA E-EDID 1.3/1.4), the facts the Graphics & displays section shows
// (#1519): the manufacturer and product code, the monitor name and serial from the display descriptors,
// and the physical image size. Pure and standard library only: Windows reads the bytes from the monitor's
// device registry key, Linux from /sys/class/drm/*/edid, and tests/fuzz/fuzz_edid.cpp fuzzes it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <optional>
#include <span>
#include <string>

namespace Platform::Edid
{

/// The base block's size; extension blocks follow it and aren't read.
inline constexpr std::size_t BLOCK_BYTES = 128;

struct EdidInfo
{
    std::string manufacturerId; ///< The three-letter PNP id ("DEL")
    std::uint16_t productCode = 0;
    std::uint32_t serialNumber = 0; ///< The numeric serial; 0 when unset
    std::string name;               ///< The monitor name descriptor (0xFC)
    std::string serial;             ///< The serial string descriptor (0xFF)
    std::uint32_t widthMm = 0;      ///< The image size; 0 when unknown
    std::uint32_t heightMm = 0;

    /// The monitor name, else "DEL 41A8" from the manufacturer and product code.
    [[nodiscard]] std::string displayName() const
    {
        return name.empty() ? std::format("{} {:04X}", manufacturerId, productCode) : name;
    }

    /// The serial string, else the numeric serial, else empty.
    [[nodiscard]] std::string serialText() const
    {
        if (!serial.empty() || serialNumber == 0)
        {
            return serial;
        }
        return std::to_string(serialNumber);
    }
};

/// A display descriptor's text: up to 13 bytes, ended by a line feed, trailing spaces dropped. A byte
/// outside printable ASCII ends it too.
[[nodiscard]] inline std::string descriptorText(std::span<const std::uint8_t> text)
{
    std::string value;
    for (const std::uint8_t byte : text)
    {
        if (byte < 0x20 || byte > 0x7E)
        {
            break;
        }
        value.push_back(static_cast<char>(byte));
    }
    while (!value.empty() && value.back() == ' ')
    {
        value.pop_back();
    }
    return value;
}

/// The base block's facts; nullopt when it is shorter than 128 bytes, has the wrong header or fails its
/// checksum (an unreadable or corrupt EDID tells nothing reliable).
[[nodiscard]] inline std::optional<EdidInfo> parseEdid(std::span<const std::uint8_t> bytes)
{
    constexpr std::array<std::uint8_t, 8> HEADER{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (bytes.size() < BLOCK_BYTES)
    {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> block = bytes.first(BLOCK_BYTES);
    if (!std::equal(HEADER.begin(), HEADER.end(), block.begin()) || std::accumulate(block.begin(), block.end(), 0U) % 256U != 0U)
    {
        return std::nullopt;
    }

    EdidInfo info;
    const unsigned packed = (static_cast<unsigned>(block[8]) << 8U) | block[9];
    for (const unsigned shift : {10U, 5U, 0U})
    {
        const unsigned letter = (packed >> shift) & 0x1FU;
        info.manufacturerId.push_back(letter >= 1 && letter <= 26 ? static_cast<char>('A' + letter - 1) : '?');
    }
    info.productCode = static_cast<std::uint16_t>(block[10] | (block[11] << 8U));
    info.serialNumber = static_cast<std::uint32_t>(block[12]) | (static_cast<std::uint32_t>(block[13]) << 8U) |
                        (static_cast<std::uint32_t>(block[14]) << 16U) | (static_cast<std::uint32_t>(block[15]) << 24U);
    // Bytes 21 and 22 are the size in cm; one of them 0 means an aspect ratio or a projector.
    if (block[21] != 0 && block[22] != 0)
    {
        info.widthMm = block[21] * 10U;
        info.heightMm = block[22] * 10U;
    }

    constexpr std::size_t FIRST_DESCRIPTOR = 54;
    constexpr std::size_t DESCRIPTOR_BYTES = 18;
    for (std::size_t offset = FIRST_DESCRIPTOR; offset + DESCRIPTOR_BYTES <= 126; offset += DESCRIPTOR_BYTES)
    {
        const std::span<const std::uint8_t> descriptor = block.subspan(offset, DESCRIPTOR_BYTES);
        if (descriptor[0] != 0 || descriptor[1] != 0)
        {
            // A detailed timing: the first one's image size in mm is finer than the cm above.
            const std::uint32_t width = descriptor[12] | ((descriptor[14] & 0xF0U) << 4U);
            const std::uint32_t height = descriptor[13] | ((descriptor[14] & 0x0FU) << 8U);
            if (offset == FIRST_DESCRIPTOR && width != 0 && height != 0)
            {
                info.widthMm = width;
                info.heightMm = height;
            }
            continue;
        }
        if (descriptor[3] == 0xFC && info.name.empty())
        {
            info.name = descriptorText(descriptor.subspan(5));
        }
        else if (descriptor[3] == 0xFF && info.serial.empty())
        {
            info.serial = descriptorText(descriptor.subspan(5));
        }
    }
    return info;
}

} // namespace Platform::Edid
