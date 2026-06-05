#pragma once

// Keep this header parseable on non-Linux platforms (e.g. Windows clangd)
// by exposing the helpers only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace Platform::ProcParsing
{

/// Read a /proc or /sys virtual file using low-level POSIX I/O.
/// Avoids std::ifstream overhead (locale machinery, sentry, streambuf allocations).
/// Returns bytes read, or 0 on failure. Buffer is NOT null-terminated.
[[nodiscard]] inline std::size_t readProcFile(const char* path, char* buf, std::size_t bufSize) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return 0;
    }
    const auto n = ::read(fd, buf, bufSize);
    ::close(fd);
    return (n > 0) ? static_cast<std::size_t>(n) : 0;
}

/// Read an entire /proc or /sys virtual file into a heap buffer, growing until EOF.
/// Avoids the fixed-size truncation of readProcFile for files without a known upper bound.
/// Returns the bytes read, or an empty vector on failure.
[[nodiscard]] inline std::vector<char> readProcFileFull(const char* path)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return {};
    }
    std::vector<char> buf;
    buf.reserve(4096);
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const auto n = ::read(fd, chunk.data(), chunk.size());
        if (n <= 0)
        {
            break;
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + static_cast<std::size_t>(n));
    }
    ::close(fd);
    return buf;
}

/// Skip ASCII space/tab characters, returning the updated pointer.
[[nodiscard]] constexpr const char* skipSpaces(const char* p, const char* end) noexcept
{
    while (p < end && (*p == ' ' || *p == '\t'))
    {
        ++p;
    }
    return p;
}

/// Parse one integer of type T from [p, end), skipping leading spaces.
/// Advances p past the parsed digits on success; leaves p unchanged and returns false on failure.
template<std::integral T> bool parseNum(const char*& p, const char* end, T& out) noexcept
{
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        return false;
    }
    p = ptr;
    return true;
}

/// Parse one double from [p, end), skipping leading spaces.
/// Advances p past the parsed number on success; returns false on failure.
inline bool parseDouble(const char*& p, const char* end, double& out) noexcept
{
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        return false;
    }
    p = ptr;
    return true;
}

} // namespace Platform::ProcParsing

#endif // defined(__linux__) && __has_include(<unistd.h>)
