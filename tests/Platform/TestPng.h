#pragma once

// Writes small PNGs for the icon tests (#1800): an RGBA image's pixels in one stored (uncompressed)
// deflate block, so no encoder is needed. Shared by test_LinuxProcessIconSource.cpp and
// test_WindowsPackageLogo.cpp.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string_view>
#include <vector>

namespace Platform::TestSupport
{

/// CRC-32 (PNG's chunk checksum).
[[nodiscard]] inline std::uint32_t pngCrc32(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    std::size_t index = 0;
    while (index < bytes.size())
    {
        crc ^= bytes[index];
        int bit = 0;
        while (bit < 8)
        {
            crc = (crc & 1U) != 0U ? (0xEDB88320U ^ (crc >> 1U)) : (crc >> 1U);
            ++bit;
        }
        ++index;
    }
    return ~crc;
}

inline void appendBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

inline void appendPngChunk(std::vector<std::uint8_t>& png, std::string_view type, const std::vector<std::uint8_t>& data)
{
    appendBigEndian(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> typed(type.begin(), type.end());
    typed.insert(typed.end(), data.begin(), data.end());
    png.insert(png.end(), typed.begin(), typed.end());
    appendBigEndian(png, pngCrc32(typed));
}

/// A @p width x @p height RGBA PNG of @p rgba (width * height * 4 bytes, rows top to bottom). Small
/// images only: the pixels and filter bytes must fit one stored block (65535 bytes).
[[nodiscard]] inline std::vector<std::uint8_t> makePng(int width, int height, std::span<const std::uint8_t> rgba)
{
    const auto rowBytes = static_cast<std::size_t>(width) * 4U;
    std::vector<std::uint8_t> raw;
    const auto rows = static_cast<std::size_t>(height);
    std::size_t row = 0;
    while (row < rows)
    {
        raw.push_back(0); // Filter: none
        const std::span<const std::uint8_t> pixels = rgba.subspan(row * rowBytes, rowBytes);
        raw.insert(raw.end(), pixels.begin(), pixels.end());
        ++row;
    }
    std::vector<std::uint8_t> zlib{0x78, 0x01, 0x01};
    const auto length = static_cast<std::uint16_t>(raw.size());
    zlib.push_back(static_cast<std::uint8_t>(length & 0xFFU));
    zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
    const auto inverted = static_cast<std::uint16_t>(0xFFFFU - length);
    zlib.push_back(static_cast<std::uint8_t>(inverted & 0xFFU));
    zlib.push_back(static_cast<std::uint8_t>(inverted >> 8U));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    std::size_t index = 0;
    while (index < raw.size())
    {
        a = (a + raw[index]) % 65521U;
        b = (b + a) % 65521U;
        ++index;
    }
    appendBigEndian(zlib, (b << 16U) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    appendBigEndian(header, static_cast<std::uint32_t>(width));
    appendBigEndian(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    appendPngChunk(png, "IHDR", header);
    appendPngChunk(png, "IDAT", zlib);
    appendPngChunk(png, "IEND", {});
    return png;
}

/// A @p side square PNG, every pixel @p r, @p g, @p b, @p a.
[[nodiscard]] inline std::vector<std::uint8_t> makeSolidPng(int side, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    std::vector<std::uint8_t> rgba;
    rgba.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U);
    std::size_t pixel = 0;
    while (pixel < static_cast<std::size_t>(side) * static_cast<std::size_t>(side))
    {
        rgba.insert(rgba.end(), {r, g, b, a});
        ++pixel;
    }
    return makePng(side, side, rgba);
}

inline void writeBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): ofstream writes chars
              static_cast<std::streamsize>(bytes.size()));
}

} // namespace Platform::TestSupport
