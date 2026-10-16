#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

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

class InstanceLock
{
  public:
    enum class Status : std::uint8_t
    {
        Acquired,              ///< This process holds the lock
        HeldByAnotherInstance, ///< Another process holds it: TaskSmack is already running
        Unavailable,           ///< The lock file couldn't be created or locked (see error())
    };

    /// Tries to take the lock on @p lockPath without waiting, creating the file (and its
    /// directory) if needed.
    explicit InstanceLock(const std::filesystem::path& lockPath);
    ~InstanceLock();

    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    InstanceLock(InstanceLock&&) = delete;
    InstanceLock& operator=(InstanceLock&&) = delete;

    [[nodiscard]] Status status() const noexcept
    {
        return m_Status;
    }

    /// Why the lock is Unavailable; empty otherwise.
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

} // namespace App
