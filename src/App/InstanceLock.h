#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#ifdef _WIN32
namespace Platform
{
class IHandedOverFolder;
} // namespace Platform
#endif

namespace App
{

/**
 * @brief Keeps a second TaskSmack from running on the same config file (#1230).
 *
 * Two instances sharing a config could undo each other's settings: each save reads the file,
 * merges its own changes and renames a new file over it, so the instance that saves second writes
 * back what it read before the first one's save. Rather than serialise every save across
 * processes, only one instance runs per config directory. A Windows build and a WSL build on one
 * machine use different config directories, so both can still run.
 *
 * The lock is an exclusive, non-blocking lock on a file beside the config: flock() on Linux,
 * LockFileEx() on Windows. The operating system drops it when the process exits, however it
 * exits, so a crash never leaves a stale lock behind. The file itself is left in place.
 */
/// The lock file for the config directory @p configDir: one TaskSmack per config directory, so a
/// launch with its own TASKSMACK_CONFIG_DIR (#1596) gets its own lock.
[[nodiscard]] inline std::filesystem::path instanceLockPath(const std::filesystem::path& configDir)
{
    return configDir / "tasksmack.lock";
}

/// InstanceLock's test seams for handing what it creates to the config's user (#2437).
struct InstanceLockTesting
{
    std::optional<std::uint32_t> effectiveUid; ///< Instead of geteuid(): 0 acts as root
    bool failChown = false;                    ///< Make handing a created file or directory over fail
};

class InstanceLock
{
  public:
    enum class Status : std::uint8_t
    {
        Acquired,              ///< This process holds the lock
        HeldByAnotherInstance, ///< Another process holds it: TaskSmack is already running
        Unavailable,           ///< The lock file couldn't be created or locked (see error())
    };

    using Testing = InstanceLockTesting;

    /// Tries to take the lock on @p lockPath without waiting, creating the file (and its
    /// directory) if needed. Running as root with another user's config directory (sudo -E), what it
    /// creates is given to that user (#2437), or the user's next normal run couldn't open the lock.
    explicit InstanceLock(const std::filesystem::path& lockPath, const Testing& testing = {});
#ifdef _WIN32
    /// Tries to take the lock on file @p name in @p folder, a config folder an elevated restart was handed (#2728),
    /// opened only through the folder held (#3774): never by a path to it, so nothing on the way is looked up again.
    InstanceLock(const Platform::IHandedOverFolder& folder, const std::filesystem::path& name);
#endif
    ~InstanceLock();

    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    InstanceLock(InstanceLock&&) = delete;
    InstanceLock& operator=(InstanceLock&&) = delete;

    [[nodiscard]] Status status() const noexcept
    {
        return m_Status;
    }

    /// Why the lock is Unavailable; or, Acquired, why something it created couldn't be given to the
    /// config's user (#2437): this run holds the lock, but the user's next one won't be able to. Empty
    /// otherwise.
    [[nodiscard]] const std::string& error() const noexcept
    {
        return m_Error;
    }

  private:
    Status m_Status = Status::Unavailable;
    std::string m_Error;
#ifdef _WIN32
    void* m_Handle = nullptr; // HANDLE; nullptr when not held
#else
    int m_Fd = -1; // -1 when not held
#endif

#ifdef _WIN32
    /// Takes the byte-range lock on @p handle, an open lock file this object now owns.
    void lock(void* handle);
#endif
};

/// The exit code of a launch that finds another TaskSmack holding its config directory's lock
/// (#2250), returned once the "already running" message is dismissed. Non-zero, so a script that
/// launches TaskSmack (the tools/profile-* captures) can tell the launch never ran, and distinct
/// from EXIT_FAILURE (1) and from the 3 that abort() exits with on Windows. 75 is sysexits.h's
/// EX_TEMPFAIL: a temporary failure, worth retrying once the other instance has closed.
inline constexpr int ALREADY_RUNNING_EXIT_CODE = 75;

/// How a launch whose instance lock ended up in @p status ends: ALREADY_RUNNING_EXIT_CODE when another
/// instance holds the lock, nullopt to start normally. A lock that couldn't be taken at all
/// (Unavailable) doesn't stop TaskSmack starting.
[[nodiscard]] constexpr std::optional<int> exitCodeForLockStatus(const InstanceLock::Status status) noexcept
{
    if (status == InstanceLock::Status::HeldByAnotherInstance)
    {
        return ALREADY_RUNNING_EXIT_CODE;
    }
    return std::nullopt;
}

/// How often a patient acquireInstanceLock() tries the lock again, unless told otherwise.
inline constexpr std::chrono::milliseconds LOCK_RETRY_EVERY{100};

/// How a patient acquireInstanceLock() says it is still waiting (#3781): while another process holds the lock, @p report
/// is told how long it has tried, about every @p every. Either unset: nothing is reported.
struct LockWaitProgress
{
    std::function<void(std::chrono::milliseconds tried)> report;
    std::chrono::milliseconds every{0};
};

/// Takes the lock on @p lockPath; while another process holds it, tries again every @p retryEvery until
/// @p patience has passed, then returns the last try. An elevated restart (#2728) waits so for the Standard copy,
/// which holds the lock until it has saved its settings and closed: the lock itself is the hand-off, as the
/// elevated copy may not be able to open that process to wait on it. Zero @p patience: one try, as before. @p progress
/// says, while it tries, that it is still waiting.
[[nodiscard]] std::unique_ptr<InstanceLock> acquireInstanceLock(const std::filesystem::path& lockPath,
                                                                std::chrono::milliseconds patience,
                                                                std::chrono::milliseconds retryEvery = LOCK_RETRY_EVERY,
                                                                const LockWaitProgress& progress = {});

#ifdef _WIN32
/// acquireInstanceLock() on file @p name in a handed-over config folder, opened only through the folder held (#3774).
[[nodiscard]] std::unique_ptr<InstanceLock> acquireInstanceLock(const Platform::IHandedOverFolder& folder,
                                                                const std::filesystem::path& name,
                                                                std::chrono::milliseconds patience,
                                                                std::chrono::milliseconds retryEvery = LOCK_RETRY_EVERY,
                                                                const LockWaitProgress& progress = {});
#endif

} // namespace App
