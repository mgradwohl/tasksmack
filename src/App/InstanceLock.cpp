#include "InstanceLock.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

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

#include "Platform/IHandedOverFolder.h"

#include <utility>
#else
#include "App/NewFileOwner.h"
#include "Core/ConfigDirOverride.h"

#include <cerrno>
#include <optional>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace App
{

InstanceLock::InstanceLock(const std::filesystem::path& lockPath, const Testing& testing)
{
#ifndef _WIN32
    // The lock's folder, opened and made by descriptor (#4152, #4156): run as root for a user, never through a link of
    // theirs, and what's made, given to them (#2437), as UserConfig::save() and Open Themes Folder make theirs.
    const uid_t effectiveUid = testing.effectiveUid.has_value() ? static_cast<uid_t>(*testing.effectiveUid) : ::geteuid();
    auto folder = NewFileOwner::openDirectoryForUser(lockPath.has_parent_path() ? lockPath.parent_path() : std::filesystem::path("."),
                                                     Core::ConfigDirOverride::handedOver(),
                                                     effectiveUid,
                                                     testing.failChown);
    if (!folder.has_value())
    {
        m_Error = std::move(folder.error());
        return;
    }
    std::string handOverError = std::move(folder->handOverError);
    // The lock file's owner: the folder's, when root and the folder isn't root's (ownerForNewFiles()'s rule).
    std::optional<NewFileOwner::FileOwner> owner;
    if (struct stat dirInfo = {}; effectiveUid == 0 && ::fstat(folder->fd.get(), &dirInfo) == 0 && dirInfo.st_uid != 0)
    {
        owner = NewFileOwner::FileOwner{.uid = dirInfo.st_uid, .gid = dirInfo.st_gid};
    }
#else
    (void) testing;
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
#endif

#ifdef _WIN32
    // Shared access, so a backup tool or indexer opening the file can't make TaskSmack think it is
    // already running; the byte-range lock below is what excludes a second instance. The handle
    // isn't inheritable, so a process TaskSmack starts doesn't keep the lock after it exits.
    HANDLE handle = ::CreateFileW(lockPath.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_ALWAYS,
                                  // A link planted where the lock goes is opened itself, never followed: an elevated
                                  // TaskSmack with a handed-over config folder (#2728) must create nothing elsewhere.
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        m_Error = std::system_category().message(static_cast<int>(::GetLastError()));
        return;
    }
    lock(handle);
#else
    // O_CLOEXEC: a program TaskSmack starts (a file manager, say) mustn't inherit the descriptor
    // and so hold the lock after TaskSmack exits. O_NOFOLLOW: never lock, or create, a link's target. Relative to the
    // folder opened above, never by its path again (#4156).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const int fd = ::openat(folder->fd.get(), lockPath.filename().c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
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
    // The lock file too, whether this run made it or an older one left it root's: the user's next run
    // opens it read-write (#2437).
    if (owner.has_value() && (testing.failChown || ::fchown(fd, owner->uid, owner->gid) != 0) && handOverError.empty())
    {
        handOverError =
            "couldn't give " + lockPath.string() + " to its user: " + std::system_category().message(testing.failChown ? EPERM : errno);
    }
    m_Fd = fd;
    m_Status = Status::Acquired;
    m_Error = std::move(handOverError);
#endif
}

#ifdef _WIN32
InstanceLock::InstanceLock(const Platform::IHandedOverFolder& folder, const std::filesystem::path& name)
{
    // The folder is there (it was held at startup): nothing is created on the way to it, only the file itself.
    auto handle = folder.openLockFile(name);
    if (!handle.has_value())
    {
        m_Error = std::move(handle.error());
        return;
    }
    lock(*handle);
}

void InstanceLock::lock(void* handle)
{
    OVERLAPPED overlapped{};
    if (::LockFileEx(static_cast<HANDLE>(handle), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0)
    {
        const DWORD lastError = ::GetLastError();
        ::CloseHandle(static_cast<HANDLE>(handle));
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
}
#endif

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

namespace
{

/// Takes a lock with @p take; while another process holds it, tries again every @p retryEvery until @p patience
/// has passed, telling @p progress how long it has tried about every progress.every (#3781).
template<typename Take>
[[nodiscard]] std::unique_ptr<InstanceLock>
retryLock(const Take& take, std::chrono::milliseconds patience, std::chrono::milliseconds retryEvery, const LockWaitProgress& progress)
{
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + patience;
    const bool reports = progress.report && progress.every > std::chrono::milliseconds{0};
    auto nextReport = started + progress.every;
    auto lock = take();
    auto now = std::chrono::steady_clock::now();
    while (lock->status() == InstanceLock::Status::HeldByAnotherInstance && now < deadline)
    {
        std::this_thread::sleep_for(retryEvery);
        lock.reset(); // this one's file handle first, so the next try opens it afresh
        lock = take();
        now = std::chrono::steady_clock::now();
        if (reports && now >= nextReport && now < deadline && lock->status() == InstanceLock::Status::HeldByAnotherInstance)
        {
            progress.report(std::chrono::duration_cast<std::chrono::milliseconds>(now - started));
            nextReport = now + progress.every;
        }
    }
    return lock;
}

} // namespace

std::unique_ptr<InstanceLock> acquireInstanceLock(const std::filesystem::path& lockPath,
                                                  std::chrono::milliseconds patience,
                                                  std::chrono::milliseconds retryEvery,
                                                  const LockWaitProgress& progress)
{
    return retryLock([&lockPath] { return std::make_unique<InstanceLock>(lockPath); }, patience, retryEvery, progress);
}

#ifdef _WIN32
std::unique_ptr<InstanceLock> acquireInstanceLock(const Platform::IHandedOverFolder& folder,
                                                  const std::filesystem::path& name,
                                                  std::chrono::milliseconds patience,
                                                  std::chrono::milliseconds retryEvery,
                                                  const LockWaitProgress& progress)
{
    return retryLock([&folder, &name] { return std::make_unique<InstanceLock>(folder, name); }, patience, retryEvery, progress);
}
#endif
} // namespace App
