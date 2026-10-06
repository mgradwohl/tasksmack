#pragma once

// The one set of RAII owners for POSIX descriptors the Linux probes use (#1183): a file descriptor
// and a DIR* stream. Each probe used to carry its own copy (FdGuard twice, UniqueFd, DirGuard).

#if defined(__linux__) && __has_include(<unistd.h>) && __has_include(<dirent.h>)

#include <utility>

#include <dirent.h>
#include <unistd.h>

namespace Platform::Posix
{

/// Owns a POSIX file descriptor and closes it on every path, including exception paths (e.g.
/// std::vector reserve/insert can throw on OOM). A negative descriptor -- a failed open() -- owns
/// nothing. Move-only: copying would let two guards close the same descriptor.
class FdGuard
{
  public:
    explicit FdGuard(int fd) noexcept : m_Fd(fd)
    {}

    ~FdGuard() noexcept
    {
        reset();
    }

    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    FdGuard(FdGuard&& other) noexcept : m_Fd(std::exchange(other.m_Fd, -1))
    {}

    FdGuard& operator=(FdGuard&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Fd = std::exchange(other.m_Fd, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }

  private:
    void reset() noexcept
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
        m_Fd = -1;
    }

    int m_Fd;
};

/// Owns a DIR* stream (opendir()/fdopendir()) and closedir()s it on every path. nullptr owns
/// nothing. Move-only, like FdGuard.
class DirGuard
{
  public:
    explicit DirGuard(DIR* dir) noexcept : m_Dir(dir)
    {}

    ~DirGuard() noexcept
    {
        reset();
    }

    DirGuard(const DirGuard&) = delete;
    DirGuard& operator=(const DirGuard&) = delete;

    DirGuard(DirGuard&& other) noexcept : m_Dir(std::exchange(other.m_Dir, nullptr))
    {}

    DirGuard& operator=(DirGuard&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Dir = std::exchange(other.m_Dir, nullptr);
        }
        return *this;
    }

    [[nodiscard]] DIR* get() const noexcept
    {
        return m_Dir;
    }

  private:
    void reset() noexcept
    {
        if (m_Dir != nullptr)
        {
            ::closedir(m_Dir);
        }
        m_Dir = nullptr;
    }

    DIR* m_Dir;
};

} // namespace Platform::Posix

#endif
