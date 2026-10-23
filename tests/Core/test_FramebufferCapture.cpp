/// @file test_FramebufferCapture.cpp
/// @brief Core::FramebufferCapture (#2827) without a GL context: the row flip that turns OpenGL's
/// bottom-up read into a top-down image, and writePng(), whose file stb_image reads back pixel for
/// pixel -- from a directory with a non-ASCII name, as a screenshot path may have (#1648).

#include "Core/FramebufferCapture.h"
#include "Core/Utf8Path.h"

#include <gtest/gtest.h>
// NOLINTNEXTLINE(misc-include-cleaner) - stb_image.h's declarations; the implementation is Platform/StbImage.cpp
#include <stb_image.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace
{

using Core::FramebufferCapture::BYTES_PER_PIXEL;

/// A @p width x @p height RGBA image whose pixel (x, y) is {x, y, x + y, 255}: every pixel distinct.
[[nodiscard]] std::vector<std::uint8_t> gradient(const int width, const int height)
{
    std::vector<std::uint8_t> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(BYTES_PER_PIXEL));
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            pixels.push_back(static_cast<std::uint8_t>(x));
            pixels.push_back(static_cast<std::uint8_t>(y));
            pixels.push_back(static_cast<std::uint8_t>(x + y));
            pixels.push_back(UINT8_MAX);
        }
    }
    return pixels;
}

/// Row @p y of @p pixels.
[[nodiscard]] std::span<const std::uint8_t> row(const std::span<const std::uint8_t> pixels, const int width, const int y)
{
    const std::size_t rowBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(BYTES_PER_PIXEL);
    return pixels.subspan(static_cast<std::size_t>(y) * rowBytes, rowBytes);
}

// ========== The row flip ==========

TEST(FramebufferCaptureTest, FlipRowsInPlaceReversesTheRowOrder)
{
    constexpr int WIDTH = 2;
    constexpr int HEIGHT = 3;
    const std::vector<std::uint8_t> original = gradient(WIDTH, HEIGHT);
    std::vector<std::uint8_t> flipped = original;
    Core::FramebufferCapture::flipRowsInPlace(flipped, WIDTH, HEIGHT);

    for (int y = 0; y < HEIGHT; ++y)
    {
        SCOPED_TRACE(y);
        const auto expected = row(original, WIDTH, HEIGHT - 1 - y);
        const auto actual = row(flipped, WIDTH, y);
        EXPECT_TRUE(std::ranges::equal(actual, expected));
    }
}

TEST(FramebufferCaptureTest, FlipRowsInPlaceLeavesOneRowAndATooShortBufferAlone)
{
    const std::vector<std::uint8_t> oneRow = gradient(4, 1);
    std::vector<std::uint8_t> pixels = oneRow;
    Core::FramebufferCapture::flipRowsInPlace(pixels, 4, 1);
    EXPECT_EQ(pixels, oneRow);

    const std::vector<std::uint8_t> twoRows = gradient(4, 2);
    pixels = twoRows;
    Core::FramebufferCapture::flipRowsInPlace(pixels, 4, 3); // claims a third row the buffer doesn't hold
    EXPECT_EQ(pixels, twoRows);
}

// ========== PNG files ==========

/// A fresh directory with a non-ASCII name under the system's temporary directory, removed afterwards.
class FramebufferCaptureFileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::random_device random;
        constexpr int MAX_ATTEMPTS = 100;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            m_Dir = std::filesystem::temp_directory_path() / Core::utf8ToPath("tasksmack_png_снимок_" + std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(m_Dir, ec) && !ec)
            {
                return;
            }
        }
        FAIL() << "Failed to create a temp directory";
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    std::filesystem::path m_Dir;
};

TEST_F(FramebufferCaptureFileTest, WritePngRoundTripsThePixelsThroughANonAsciiPath)
{
    constexpr int WIDTH = 5;
    constexpr int HEIGHT = 3;
    const std::vector<std::uint8_t> pixels = gradient(WIDTH, HEIGHT);
    const std::filesystem::path file = m_Dir / Core::utf8ToPath("экран.png");

    const auto written = Core::FramebufferCapture::writePng(file, WIDTH, HEIGHT, pixels);
    ASSERT_TRUE(written.has_value()) << written.error();

    // Read back with stb_image, by the UTF-8 name as the app's icon loading does (STBI_WINDOWS_UTF8).
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(Core::pathToUtf8(file).c_str(), &width, &height, &channels, BYTES_PER_PIXEL);
    ASSERT_NE(decoded, nullptr) << stbi_failure_reason();
    EXPECT_EQ(width, WIDTH);
    EXPECT_EQ(height, HEIGHT);
    EXPECT_EQ(channels, BYTES_PER_PIXEL);
    const std::span<const std::uint8_t> readBack(decoded, pixels.size());
    EXPECT_TRUE(std::ranges::equal(readBack, pixels));
    stbi_image_free(decoded);
}

TEST_F(FramebufferCaptureFileTest, WritePngRefusesAnImageTheBufferDoesNotHold)
{
    const std::vector<std::uint8_t> pixels = gradient(2, 2);
    const std::filesystem::path file = m_Dir / "short.png";
    const auto written = Core::FramebufferCapture::writePng(file, 3, 2, pixels); // 3x2 needs 24 bytes, not 16
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().find("3x2"), std::string::npos) << written.error();
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(file, ec));
}

TEST_F(FramebufferCaptureFileTest, WritePngIntoAMissingDirectoryFailsNamingTheFile)
{
    const std::vector<std::uint8_t> pixels = gradient(2, 2);
    const std::filesystem::path file = m_Dir / "missing" / "shot.png";
    const auto written = Core::FramebufferCapture::writePng(file, 2, 2, pixels);
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().find("shot.png"), std::string::npos) << written.error();
}

} // namespace
