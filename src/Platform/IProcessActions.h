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
    bool canTerminate = false; // SIGTERM
    bool canKill = false;      // SIGKILL
    bool canStop = false;      // SIGSTOP
    bool canContinue = false;  // SIGCONT
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
};

} // namespace Platform
