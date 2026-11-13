/// @file test_Sha256.cpp
/// @brief SHA-256 against the FIPS 180-4 and NIST vectors, and hashFile() (#3094).

#include "Platform/Sha256.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::Sha256
{
namespace
{

TEST(Sha256Test, MatchesTheFipsAndNistVectors)
{
    EXPECT_EQ(toHex(of("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(toHex(of("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(toHex(of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(toHex(of(std::string(1000000, 'a'))), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Inputs at and around the 56- and 64-byte padding edges: the same digest whether given whole or a byte at a time.
TEST(Sha256Test, PiecesGiveTheSameDigestAsTheWhole)
{
    for (const std::size_t length : {55U, 56U, 57U, 63U, 64U, 65U, 119U, 120U, 128U, 1000U})
    {
        const std::string input(length, 'x');
        Hasher pieces;
        for (const char c : input)
        {
            pieces.update(std::string_view(&c, 1));
        }
        EXPECT_EQ(pieces.finish(), of(input)) << length;
    }
    EXPECT_EQ(toHex(of(std::string(64, 'x'))), "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c");
}

TEST(Sha256Test, HashesAFileAndSaysWhyItCant)
{
    // A shared test: its own temp file, as the Linux-only ScopedTempDir isn't built on Windows.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("tasksmack-3094-sha256-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    const auto path = dir / "file.bin";
    {
        std::ofstream(path, std::ios::binary) << "abc";
    }
    const auto digest = hashFile(path, std::stop_token{});
    ASSERT_TRUE(digest.has_value()) << digest.error();
    EXPECT_EQ(toHex(*digest), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const auto missing = hashFile(dir / "missing", std::stop_token{});
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(missing.error().starts_with("couldn't open the file")) << missing.error();

    std::stop_source stop;
    stop.request_stop();
    const auto stopped = hashFile(path, stop.get_token());
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error(), "Stopped");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace Platform::Sha256
