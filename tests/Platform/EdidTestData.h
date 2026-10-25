#pragma once

/// @file EdidTestData.h
/// @brief A real-looking 128-byte EDID base block for the EDID parser's and the graphics readers' tests
/// (#1519): a Dell ("DEL", product 0x41A8) with a detailed timing giving 597 x 336 mm, then optional name
/// and serial descriptors, and a valid checksum.

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <vector>

namespace Platform::TestSupport
{

inline void putDescriptorText(std::vector<std::uint8_t>& edid, std::size_t offset, std::uint8_t tag, std::string_view text)
{
    edid[offset + 3] = tag;
    for (std::size_t i = 0; i < 13; ++i)
    {
        std::uint8_t byte = 0x20;
        if (i < text.size())
        {
            byte = static_cast<std::uint8_t>(text[i]);
        }
        else if (i == text.size())
        {
            byte = 0x0A;
        }
        edid[offset + 5 + i] = byte;
    }
}

/// The block, with a name (0xFC) and serial (0xFF) descriptor when they aren't empty, and the detailed
/// timing when @p withTiming.
[[nodiscard]] inline std::vector<std::uint8_t>
makeEdid(std::string_view name = "DELL U2720Q", std::string_view serial = "ABC1234", bool withTiming = true)
{
    std::vector<std::uint8_t> edid(128, 0);
    for (std::size_t i = 1; i < 7; ++i)
    {
        edid[i] = 0xFF;
    }
    edid[8] = 0x10; // "DEL": (4 << 10) | (5 << 5) | 12
    edid[9] = 0xAC;
    edid[10] = 0xA8; // product 0x41A8, little-endian
    edid[11] = 0x41;
    edid[12] = 0x01; // numeric serial 0x04030201
    edid[13] = 0x02;
    edid[14] = 0x03;
    edid[15] = 0x04;
    edid[18] = 1; // EDID 1.4
    edid[19] = 4;
    edid[21] = 60; // 60 x 34 cm
    edid[22] = 34;
    if (withTiming)
    {
        edid[54] = 0x4D; // a non-zero pixel clock: a detailed timing
        edid[55] = 0xD0;
        edid[66] = static_cast<std::uint8_t>(597U & 0xFFU);
        edid[67] = static_cast<std::uint8_t>(336U & 0xFFU);
        edid[68] = static_cast<std::uint8_t>(((597U >> 8U) << 4U) | (336U >> 8U));
    }
    if (!name.empty())
    {
        putDescriptorText(edid, 72, 0xFC, name);
    }
    if (!serial.empty())
    {
        putDescriptorText(edid, 90, 0xFF, serial);
    }
    edid[108 + 3] = 0x10; // a dummy descriptor
    const unsigned sum = std::accumulate(edid.begin(), edid.end() - 1, 0U);
    edid[127] = static_cast<std::uint8_t>((256U - (sum % 256U)) % 256U);
    return edid;
}

} // namespace Platform::TestSupport
