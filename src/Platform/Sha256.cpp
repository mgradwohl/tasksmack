#include "Sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::Sha256
{

namespace
{

// FIPS 180-4 §4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> ROUND_CONSTANTS{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::size_t READ_CHUNK_BYTES = 1U << 20U; // 1 MiB a read: a stop is seen within one chunk

} // namespace

void Hasher::compress(const std::uint8_t* block) noexcept
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        w[i] = (std::uint32_t{block[4 * i]} << 24U) | (std::uint32_t{block[(4 * i) + 1]} << 16U) |
               (std::uint32_t{block[(4 * i) + 2]} << 8U) | std::uint32_t{block[(4 * i) + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = m_State[0];
    std::uint32_t b = m_State[1];
    std::uint32_t c = m_State[2];
    std::uint32_t d = m_State[3];
    std::uint32_t e = m_State[4];
    std::uint32_t f = m_State[5];
    std::uint32_t g = m_State[6];
    std::uint32_t h = m_State[7];
    for (std::size_t i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + choice + ROUND_CONSTANTS[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_State[0] += a;
    m_State[1] += b;
    m_State[2] += c;
    m_State[3] += d;
    m_State[4] += e;
    m_State[5] += f;
    m_State[6] += g;
    m_State[7] += h;
}

void Hasher::update(std::span<const std::uint8_t> data) noexcept
{
    m_TotalBytes += data.size();
    while (!data.empty())
    {
        const std::size_t take = std::min(data.size(), m_Block.size() - m_BlockUsed);
        std::memcpy(m_Block.data() + m_BlockUsed, data.data(), take);
        m_BlockUsed += take;
        data = data.subspan(take);
        if (m_BlockUsed == m_Block.size())
        {
            compress(m_Block.data());
            m_BlockUsed = 0;
        }
    }
}

void Hasher::update(std::string_view text) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a char's bytes as the unsigned bytes they are
    update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Digest Hasher::finish() noexcept
{
    const std::uint64_t bitLength = m_TotalBytes * 8U;
    // FIPS 180-4 §5.1.1: a 1 bit, zeros to 56 mod 64 bytes, then the length in bits, big-endian.
    m_Block[m_BlockUsed++] = 0x80U;
    if (m_BlockUsed > 56)
    {
        std::fill(m_Block.begin() + static_cast<std::ptrdiff_t>(m_BlockUsed), m_Block.end(), std::uint8_t{0});
        compress(m_Block.data());
        m_BlockUsed = 0;
    }
    std::fill(m_Block.begin() + static_cast<std::ptrdiff_t>(m_BlockUsed), m_Block.begin() + 56, std::uint8_t{0});
    for (std::size_t i = 0; i < 8; ++i)
    {
        m_Block[56 + i] = static_cast<std::uint8_t>(bitLength >> (56U - (8U * i)));
    }
    compress(m_Block.data());
    Digest digest{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        digest[4 * i] = static_cast<std::uint8_t>(m_State[i] >> 24U);
        digest[(4 * i) + 1] = static_cast<std::uint8_t>(m_State[i] >> 16U);
        digest[(4 * i) + 2] = static_cast<std::uint8_t>(m_State[i] >> 8U);
        digest[(4 * i) + 3] = static_cast<std::uint8_t>(m_State[i]);
    }
    return digest;
}

Digest of(std::string_view data) noexcept
{
    Hasher hasher;
    hasher.update(data);
    return hasher.finish();
}

std::string toHex(const Digest& digest)
{
    constexpr std::string_view HEX = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest)
    {
        text += HEX[byte >> 4U];
        text += HEX[byte & 0x0FU];
    }
    return text;
}

std::expected<Digest, std::string> hashFile(const std::filesystem::path& path, const std::stop_token& stop)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        const std::error_code ec(errno, std::generic_category());
        return std::unexpected(std::string("couldn't open the file: ") + ec.message());
    }
    Hasher hasher;
    std::vector<char> chunk(READ_CHUNK_BYTES);
    while (file)
    {
        if (stop.stop_requested())
        {
            return std::unexpected(std::string("Stopped"));
        }
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = file.gcount();
        if (got > 0)
        {
            hasher.update(std::string_view(chunk.data(), static_cast<std::size_t>(got)));
        }
    }
    if (file.bad())
    {
        return std::unexpected(std::string("couldn't read the file"));
    }
    return hasher.finish();
}

} // namespace Platform::Sha256
