#pragma once

// SHA-256 (FIPS 180-4), for "Look up on VirusTotal" (#3094): the digest of a process's executable, which is all
// TaskSmack sends. Portable and dependency-free: the same code on Windows and Linux. The digest is pure (tested
// against the FIPS and NIST vectors); hashFile() reads a file in chunks off the UI thread and can be stopped.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform::Sha256
{

/// A digest: 32 bytes.
using Digest = std::array<std::uint8_t, 32>;

/// An incremental SHA-256: update() with the input in any number of pieces, then finish() once.
class Hasher
{
  public:
    void update(std::span<const std::uint8_t> data) noexcept;
    void update(std::string_view text) noexcept;
    /// The digest of everything given to update(). The hasher is spent afterwards.
    [[nodiscard]] Digest finish() noexcept;

  private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> m_State{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> m_Block{};
    std::size_t m_BlockUsed = 0;
    std::uint64_t m_TotalBytes = 0;
};

/// @p data's digest in one call.
[[nodiscard]] Digest of(std::string_view data) noexcept;

/// @p digest as 64 lowercase hex digits, as VirusTotal and sha256sum write it.
[[nodiscard]] std::string toHex(const Digest& digest);

/// The digest of the file at @p path, read in chunks. Stops early, with "Stopped", once @p stop is requested; the
/// error otherwise says what couldn't be read. Blocking: call it off the UI thread.
[[nodiscard]] std::expected<Digest, std::string> hashFile(const std::filesystem::path& path, const std::stop_token& stop);

} // namespace Platform::Sha256
