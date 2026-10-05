#include "InstanceLock.h"

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#else
#include <cerrno>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace App
{

InstanceLock::InstanceLock(const std::filesystem::path& lockPath)
{
    std::error_code ec;
    if (lockPath.has_parent_path())
    {
        std::filesystem::create_directories(lockPath.parent_path(), ec);
        if (ec)
        {
            m_Error = ec.message();
            return;
        }
    }

#ifdef _WIN32
    // Shared access, so a backup tool or indexer opening the file can't make TaskSmack think it is
    // already running; the byte-range lock below is what excludes a second instance. The handle
    // isn't inheritable, so a process TaskSmack starts doesn't keep the lock after it exits.
    HANDLE handle = ::CreateFileW(lockPath.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        m_Error = std::system_category().message(static_cast<int>(::GetLastError()));
        return;
    }
    OVERLAPPED overlapped{};
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0)
    {
        const DWORD lastError = ::GetLastError();
        ::CloseHandle(handle);
        if (lastError == ERROR_LOCK_VIOLATION)
        {
            m_Status = Status::HeldByAnotherInstance;
        }
        else
        {
            m_Error = std::system_category().message(static_cast<int>(lastError));
        }
        return;
    }
    m_Handle = handle;
    m_Status = Status::Acquired;
#else
    // O_CLOEXEC: a program TaskSmack starts (a file manager, say) mustn't inherit the descriptor
    // and so hold the lock after TaskSmack exits. O_NOFOLLOW: never lock, or create, a link's target.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        m_Error = std::system_category().message(errno);
        return;
    }
    int result = ::flock(fd, LOCK_EX | LOCK_NB);
    while (result != 0 && errno == EINTR)
    {
        result = ::flock(fd, LOCK_EX | LOCK_NB);
    }
    if (result != 0)
    {
        const int lockError = errno;
        ::close(fd);
        if (lockError == EWOULDBLOCK)
        {
            m_Status = Status::HeldByAnotherInstance;
        }
        else
        {
            m_Error = std::system_category().message(lockError);
        }
        return;
    }
    m_Fd = fd;
    m_Status = Status::Acquired;
#endif
}

InstanceLock::~InstanceLock()
{
#ifdef _WIN32
    if (m_Handle != nullptr)
    {
        ::CloseHandle(static_cast<HANDLE>(m_Handle)); // releases the lock
    }
#else
    if (m_Fd >= 0)
    {
        ::close(m_Fd); // releases the lock
    }
#endif
}

} // namespace App
