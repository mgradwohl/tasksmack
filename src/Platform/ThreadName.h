#pragma once

// OS thread names, so per-thread tools (pidstat -t, top -H, perf, gdb, WPA, Visual Studio's
// Threads window) show which TaskSmack thread is which (#843 measurement kit). In Platform, the
// lowest layer, so Core and Domain can both name their threads without a layering violation.

#include <cstddef>
#include <string>
#include <string_view>

namespace Platform
{

/// Linux's limit for a thread name (TASK_COMM_LEN 16, including the terminating NUL);
/// pthread_setname_np() fails with ERANGE on anything longer.
inline constexpr std::size_t MAX_LINUX_THREAD_NAME_LENGTH = 15;

/// Thread names TaskSmack gives its own threads. Each fits MAX_LINUX_THREAD_NAME_LENGTH, so the
/// name a tool shows is the whole name on every platform.
inline constexpr std::string_view UI_THREAD_NAME = "tasksmack-ui";
inline constexpr std::string_view SAMPLER_THREAD_NAME = "ts-sampler";
inline constexpr std::string_view PROCESS_SAMPLER_THREAD_NAME = "ts-sampler-proc";
inline constexpr std::string_view SYSTEM_SAMPLER_THREAD_NAME = "ts-sampler-sys";
inline constexpr std::string_view SERVICE_SAMPLER_THREAD_NAME = "ts-sampler-svc";
/// The worker that runs one Process Details Connections read at a time (#799).
inline constexpr std::string_view CONNECTIONS_READ_THREAD_NAME = "ts-conn-read";

/// `name` cut to at most `maxLength` bytes, never in the middle of a UTF-8 sequence.
[[nodiscard]] inline std::string truncateThreadName(std::string_view name, std::size_t maxLength = MAX_LINUX_THREAD_NAME_LENGTH)
{
    if (name.size() <= maxLength)
    {
        return std::string(name);
    }
    std::size_t length = maxLength;
    // Back up over continuation bytes (10xxxxxx) so the cut lands on a code point boundary.
    while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0U) == 0x80U)
    {
        --length;
    }
    return std::string(name.substr(0, length));
}

/// Names the calling thread (Linux: pthread_setname_np, truncated to
/// MAX_LINUX_THREAD_NAME_LENGTH; Windows: SetThreadDescription). Best effort: returns false if the
/// OS refused, and never throws.
bool setCurrentThreadName(std::string_view name) noexcept;

/// Names the main (UI) thread. On Linux this is deliberately a no-op that returns true: the main
/// thread's name IS the process name (/proc/<pid>/comm), which ps, top, pgrep and TaskSmack's own
/// process list show, so renaming it would rename the process. Its TID equals the PID, which is
/// how per-thread tools tell it apart there. On Windows it sets the thread description, which
/// leaves the process name alone.
bool setMainThreadName(std::string_view name) noexcept;

} // namespace Platform
