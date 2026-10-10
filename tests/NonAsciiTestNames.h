#pragma once

/// @file NonAsciiTestNames.h
/// @brief Non-ASCII names for the UTF-8 round-trip tests (#1648, slice C), and a fresh temporary
/// directory named with all of them.
///
/// The four names cover what breaks a narrow or code-page conversion: CJK (outside every Western
/// code page), an emoji outside the BMP (a UTF-16 surrogate pair, four UTF-8 bytes), accented Latin
/// (inside code page 1252, so a 1252 conversion "works" but yields bytes that aren't UTF-8), and
/// right-to-left Arabic. The test binaries have no application manifest, so on Windows they run in
/// the system's ANSI code page (often 1252), not UTF-8: a test that passes here proves the code
/// doesn't depend on the manifest's activeCodePage.

#include "Core/Utf8Path.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace TestSupport
{

inline constexpr std::string_view CJK_NAME = "任务管理器";
inline constexpr std::string_view EMOJI_NAME = "🔥";
inline constexpr std::string_view LATIN_NAME = "Ünïcödé";
inline constexpr std::string_view RTL_NAME = "مدير";

inline constexpr std::array NON_ASCII_NAMES{CJK_NAME, EMOJI_NAME, LATIN_NAME, RTL_NAME};

/// All four names in one string.
[[nodiscard]] inline std::string allNonAsciiNames()
{
    return std::format("{}{}{}{}", CJK_NAME, EMOJI_NAME, LATIN_NAME, RTL_NAME);
}

/// A new, empty directory under the system temp directory, named with all four names plus a
/// unique suffix, removed with everything in it on destruction. created() is false when the
/// filesystem refused the name (a test should then GTEST_SKIP: it can't test what can't exist).
class NonAsciiTempDir
{
  public:
    NonAsciiTempDir()
    {
        static std::atomic<std::uint32_t> counter{0};
        std::random_device random;
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec)
        {
            return;
        }
        int attempt = 0;
        while (attempt < MAX_ATTEMPTS)
        {
            ++attempt;
            const std::string name =
                std::format("tasksmack-{}-{:08x}-{}", allNonAsciiNames(), random(), counter.fetch_add(1, std::memory_order_relaxed));
            const std::filesystem::path candidate = base / Core::utf8ToPath(name);
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_Path = candidate;
                return;
            }
        }
    }

    ~NonAsciiTempDir()
    {
        if (!m_Path.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Path, ec); // best effort; never throws from a destructor
        }
    }

    NonAsciiTempDir(const NonAsciiTempDir&) = delete;
    NonAsciiTempDir& operator=(const NonAsciiTempDir&) = delete;
    NonAsciiTempDir(NonAsciiTempDir&&) = delete;
    NonAsciiTempDir& operator=(NonAsciiTempDir&&) = delete;

    [[nodiscard]] bool created() const noexcept
    {
        return !m_Path.empty();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    static constexpr int MAX_ATTEMPTS = 16;
    std::filesystem::path m_Path;
};

} // namespace TestSupport
