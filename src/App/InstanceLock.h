#pragma once

#include <cstdint>
#include <filesystem>
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

} // namespace App
