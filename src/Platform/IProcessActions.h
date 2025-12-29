#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace Platform
{

/// Result of a process action (kill, terminate, etc.)
struct ProcessActionResult
{
    bool success = false;
    std::string errorMessage;

    static ProcessActionResult ok()
    {
        return {.success = true, .errorMessage = {}};
    }
    static ProcessActionResult error(std::string msg)
    {
        return {.success = false, .errorMessage = std::move(msg)};
    }
};

/// Capabilities for process actions.
struct ProcessActionCapabilities
{
    bool canTerminate = false;   // SIGTERM
    bool canKill = false;        // SIGKILL
    bool canStop = false;        // SIGSTOP
    bool canContinue = false;    // SIGCONT
    bool canSetPriority = false; // setpriority/SetPriorityClass
};

/// Interface for platform-specific process actions.
class IProcessActions
{
  public:
    virtual ~IProcessActions() = default;

    IProcessActions() = default;
    IProcessActions(const IProcessActions&) = default;
    IProcessActions& operator=(const IProcessActions&) = default;
    IProcessActions(IProcessActions&&) = default;
    IProcessActions& operator=(IProcessActions&&) = default;

    /// What actions this platform supports.
    [[nodiscard]] virtual ProcessActionCapabilities actionCapabilities() const = 0;

    /// Send SIGTERM (graceful termination request).
    [[nodiscard]] virtual ProcessActionResult terminate(int32_t pid) = 0;

    /// Send SIGKILL (forceful kill).
    [[nodiscard]] virtual ProcessActionResult kill(int32_t pid) = 0;

    /// Send SIGSTOP (pause process).
    [[nodiscard]] virtual ProcessActionResult stop(int32_t pid) = 0;

    /// Send SIGCONT (resume paused process).
    [[nodiscard]] virtual ProcessActionResult resume(int32_t pid) = 0;

    /// Set process priority (nice value on Unix, priority class on Windows).
    /// @param pid Process ID
    /// @param nice Nice value (-20 to 19 on Unix, mapped to priority class on Windows)
    [[nodiscard]] virtual ProcessActionResult setPriority(int32_t pid, int32_t nice) = 0;
};

} // namespace Platform
