#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <system_error>

#if defined(__linux__) && __has_include(<unistd.h>)
#include <array>
#include <cerrno>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace Platform::ProcParsing
{

#if defined(__linux__) && __has_include(<unistd.h>)

/// RAII guard for a POSIX file descriptor. Ensures the fd is closed on all paths,
/// including exception paths (e.g. std::vector reserve/insert can throw on OOM).
/// Move-only: copying would let two guards close the same fd (double-close/UB), so the
/// copy operations are deleted rather than left to the (unsafe) compiler-generated default.
class FdGuard
{
  public:
    explicit FdGuard(int fd) noexcept : m_Fd(fd)
    {}

    ~FdGuard() noexcept
    {
        if (m_Fd != -1)
        {
            ::close(m_Fd);
        }
    }

    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    FdGuard(FdGuard&& other) noexcept : m_Fd(std::exchange(other.m_Fd, -1))
    {}

    FdGuard& operator=(FdGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Fd != -1)
            {
                ::close(m_Fd);
            }
            m_Fd = std::exchange(other.m_Fd, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }

  private:
    int m_Fd;
};

/// Read a /proc or /sys virtual file using low-level POSIX I/O.
/// Avoids std::ifstream overhead (locale machinery, sentry, streambuf allocations).
/// Loops to guard against short reads (POSIX allows ::read to return less than requested).
/// Returns bytes read, or 0 on failure. Buffer is NOT null-terminated.
[[nodiscard]] inline std::size_t readProcFile(const char* path, char* buf, std::size_t bufSize) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — POSIX open() is variadic
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
        return 0;
    }
    std::size_t total = 0;
    bool readError = false;
    while (total < bufSize)
    {
        const auto n = ::read(fd, buf + total, bufSize - total);
        if (n == 0)
        {
            break; // EOF
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // interrupted by signal — retry
            }
            readError = true;
            break; // I/O error — discard partial data
        }
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return readError ? 0 : total;
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
    const FdGuard guard{fd}; // ensures fd is closed on all paths, including exception paths
                             // (buf.reserve / buf.insert can throw on OOM)

    std::vector<char> buf;
    buf.reserve(4096);
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const auto n = ::read(fd, chunk.data(), chunk.size());
        if (n == 0)
        {
            break; // EOF
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // interrupted by signal — retry
            }
            return {}; // I/O error — discard partial data
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + static_cast<std::size_t>(n));
    }
    return buf;
}

#endif

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
    const char* const saved = p;
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        p = saved;
        return false;
    }
    p = ptr;
    return true;
}

/// Parse one double from [p, end), skipping leading spaces.
/// Advances p past the parsed number on success; leaves p unchanged and returns false on failure.
inline bool parseDouble(const char*& p, const char* end, double& out) noexcept
{
    const char* const saved = p;
    p = skipSpaces(p, end);
    const auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{})
    {
        p = saved;
        return false;
    }
    p = ptr;
    return true;
}

} // namespace Platform::ProcParsing
