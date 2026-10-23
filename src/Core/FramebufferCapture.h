#pragma once

// A PNG of the window's framebuffer (#2827): the default framebuffer's pixels, read back with
// glReadPixels once a frame is fully drawn and before it is presented, and written with
// stb_image_write. The one OpenGL read a screenshot needs, kept in Core so the App layer that asks
// for one (App/ScreenshotLayer) never touches GL itself. flipRowsInPlace() is pure and unit-tested
// with writePng() (tests/Core/test_FramebufferCapture.cpp); the read needs a current GL context
// (tests/App/test_ScreenshotOnDisplay.cpp runs it on a display).

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Core::FramebufferCapture
{

/// Straight RGBA, 8 bits each.
inline constexpr int BYTES_PER_PIXEL = 4;

/// Turns the bottom-up rows OpenGL reads into top-down ones, in place. @p pixels holds
/// @p width x @p height x BYTES_PER_PIXEL bytes; fewer, and nothing is changed.
void flipRowsInPlace(std::span<std::uint8_t> pixels, int width, int height);

/// Reads the default framebuffer's @p width x @p height pixels into @p outRgba as straight RGBA, rows
/// top-down and every pixel opaque (the window is opaque; its buffer's alpha is only what blending
/// left). The window's GL context must be current, the frame fully drawn and the buffers not yet
/// swapped: a presented back buffer's content is undefined. @p outRgba is resized, so a caller
/// capturing a sequence keeps one allocation. Leaves the pack alignment and read framebuffer binding
/// as they were. The error text on failure.
[[nodiscard]] std::expected<void, std::string> readDefaultFramebuffer(int width, int height, std::vector<std::uint8_t>& outRgba);

/// Writes @p rgba (@p width x @p height x BYTES_PER_PIXEL bytes, rows top-down) as a PNG at @p path,
/// creating or replacing the file; the directory must exist. The error text on failure.
[[nodiscard]] std::expected<void, std::string>
writePng(const std::filesystem::path& path, int width, int height, std::span<const std::uint8_t> rgba);

} // namespace Core::FramebufferCapture
