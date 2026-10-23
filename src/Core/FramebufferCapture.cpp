#include "FramebufferCapture.h"

#include "Core/Utf8Path.h"

#include <glad/gl.h>
// NOLINTNEXTLINE(misc-include-cleaner) - stb_image_write.h's declarations; the implementation is Platform/StbImage.cpp
#include <stb_image_write.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace Core::FramebufferCapture
{

namespace
{

/// glGetError() clears one flag per call; this many are drained before a read so an earlier error
/// isn't taken for the read's own.
constexpr int MAX_PENDING_GL_ERRORS = 8;
constexpr std::uint8_t OPAQUE_ALPHA = 0xFF; // not OPAQUE: a Windows GDI macro
constexpr std::size_t ALPHA_OFFSET = 3;

[[nodiscard]] std::size_t byteCount(const int width, const int height)
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(BYTES_PER_PIXEL);
}

} // namespace

void flipRowsInPlace(const std::span<std::uint8_t> pixels, const int width, const int height)
{
    if (width <= 0 || height <= 1 || pixels.size() < byteCount(width, height))
    {
        return;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(BYTES_PER_PIXEL);
    for (int row = 0; row < height / 2; ++row)
    {
        const std::span<std::uint8_t> top = pixels.subspan(static_cast<std::size_t>(row) * rowBytes, rowBytes);
        const std::span<std::uint8_t> bottom = pixels.subspan(static_cast<std::size_t>(height - 1 - row) * rowBytes, rowBytes);
        std::ranges::swap_ranges(top, bottom);
    }
}

std::expected<void, std::string> readDefaultFramebuffer(const int width, const int height, std::vector<std::uint8_t>& outRgba)
{
    if (width <= 0 || height <= 0)
    {
        return std::unexpected(std::format("the window has no drawable area ({}x{})", width, height));
    }
    outRgba.resize(byteCount(width, height));

    for (int i = 0; i < MAX_PENDING_GL_ERRORS && glGetError() != GL_NO_ERROR; ++i)
    {
        // An error left by earlier drawing is not this read's.
    }

    // From the window's own framebuffer, tightly packed; the caller's state is put back after.
    GLint readFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    GLint packAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    if (readFramebuffer != 0)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, outRgba.data());
    const GLenum error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    if (readFramebuffer != 0)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
    }
    if (error != GL_NO_ERROR)
    {
        return std::unexpected(std::format("glReadPixels failed (GL error 0x{:04X})", error));
    }

    flipRowsInPlace(outRgba, width, height);
    for (std::size_t alpha = ALPHA_OFFSET; alpha < outRgba.size(); alpha += static_cast<std::size_t>(BYTES_PER_PIXEL))
    {
        outRgba[alpha] = OPAQUE_ALPHA;
    }
    return {};
}

std::expected<void, std::string>
writePng(const std::filesystem::path& path, const int width, const int height, const std::span<const std::uint8_t> rgba)
{
    if (width <= 0 || height <= 0 || rgba.size() < byteCount(width, height))
    {
        return std::unexpected(std::format("{} bytes is not a {}x{} RGBA image", rgba.size(), width, height));
    }
    // UTF-8, not path.string(): with STBIW_WINDOWS_UTF8 (Platform/StbImage.cpp) stb opens the file by
    // its wide name on Windows, whatever the code page (#1648).
    const std::string utf8 = pathToUtf8(path);
    errno = 0;
    if (stbi_write_png(utf8.c_str(), width, height, BYTES_PER_PIXEL, rgba.data(), width * BYTES_PER_PIXEL) == 0)
    {
        const int error = errno;
        if (error != 0)
        {
            return std::unexpected(std::format("couldn't write {}: {}", utf8, std::error_code(error, std::generic_category()).message()));
        }
        return std::unexpected(std::format("couldn't write {}", utf8));
    }
    return {};
}

} // namespace Core::FramebufferCapture
