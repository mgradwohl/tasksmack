#pragma once

// Pure mappings behind WindowsServiceActions (#1577): Win32 error codes to the words the Services
// tab shows, start types to SCM codes, and the dependents message. No Windows header, so the tests
// build on every platform (tests/Platform/WindowsMath/).

#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace Platform::Windows::ServiceActionMath
{

/// How often an action looks at the service while it waits for the new state. Short, so a stop
/// request on the action's token ends the wait within one poll (#1591).
inline constexpr std::uint32_t WAIT_POLL_MS = 250;
/// The least time a service may go without progress before the wait gives up: the whole wait for a
/// service that reports no wait hint (#2127).
inline constexpr std::uint32_t WAIT_STALL_MIN_MS = 10'000;
/// The most time the wait allows, however long the hints and however steady the progress (#2127).
inline constexpr std::uint32_t WAIT_TOTAL_MAX_MS = 180'000;

/// How long a service may go without progress, given its SERVICE_STATUS::dwWaitHint: the hint, but
/// at least WAIT_STALL_MIN_MS and at most WAIT_TOTAL_MAX_MS (#2127).
[[nodiscard]] constexpr std::uint32_t stallWindowMs(std::uint32_t waitHintMs) noexcept
{
    return std::clamp(waitHintMs, WAIT_STALL_MIN_MS, WAIT_TOTAL_MAX_MS);
}

/// What the wait does after one look at the service.
enum class WaitVerdict : std::uint8_t
{
    KeepWaiting, ///< Not there yet, and still within its time.
    Stalled,     ///< No progress for longer than its wait hint allows.
    TooLong,     ///< Making progress, but past WAIT_TOTAL_MAX_MS overall.
};

/// The SCM's documented wait for a pending service (#2127), as a pure clock the tests drive. A
/// pending service reports progress by raising dwCheckPoint, and how long the next step may take in
/// dwWaitHint; a new dwCurrentState is progress too (the checkpoint starts again from 0). The wait
/// gives up only when the service has made no progress for stallWindowMs(hint), or has taken
/// WAIT_TOTAL_MAX_MS in all, so a slow but advancing stop or start is waited out rather than reported
/// as a failure after a fixed time.
class PendingWait
{
  public:
    /// One look at the service, @p elapsedMs into the wait, with what SERVICE_STATUS reported.
    [[nodiscard]] constexpr WaitVerdict
    observe(std::uint32_t elapsedMs, std::uint32_t state, std::uint32_t checkPoint, std::uint32_t waitHintMs) noexcept
    {
        if (!m_Seen || state != m_State || checkPoint > m_CheckPoint)
        {
            m_Seen = true;
            m_State = state;
            m_CheckPoint = checkPoint;
            m_ProgressAtMs = elapsedMs;
        }
        if (elapsedMs >= WAIT_TOTAL_MAX_MS)
        {
            return WaitVerdict::TooLong;
        }
        if (elapsedMs - m_ProgressAtMs >= stallWindowMs(waitHintMs))
        {
            return WaitVerdict::Stalled;
        }
        return WaitVerdict::KeepWaiting;
    }

  private:
    bool m_Seen = false;
    std::uint32_t m_State = 0;
    std::uint32_t m_CheckPoint = 0;
    std::uint32_t m_ProgressAtMs = 0;
};

/// A Win32 error from a service call, in words the result line shows as they stand. ERROR_ACCESS_DENIED
/// is "Requires administrator": not elevated, that is nearly always why.
[[nodiscard]] inline std::string errorText(std::uint32_t code)
{
    switch (code)
    {
    case 5: // ERROR_ACCESS_DENIED
        return "Requires administrator";
    case 1051: // ERROR_DEPENDENT_SERVICES_RUNNING
        return "Other running services depend on it";
    case 1052: // ERROR_INVALID_SERVICE_CONTROL
        return "The service doesn't accept that request";
    case 1053: // ERROR_SERVICE_REQUEST_TIMEOUT
        return "The service didn't respond in time";
    case 1056: // ERROR_SERVICE_ALREADY_RUNNING
        return "It is already running";
    case 1058: // ERROR_SERVICE_DISABLED
        return "It is disabled; change its startup type first";
    case 1060: // ERROR_SERVICE_DOES_NOT_EXIST
        return "The service no longer exists";
    case 1061: // ERROR_SERVICE_CANNOT_ACCEPT_CTRL
        return "The service can't accept requests right now";
    case 1062: // ERROR_SERVICE_NOT_ACTIVE
        return "It is not running";
    case 1068: // ERROR_SERVICE_DEPENDENCY_FAIL
        return "A service it depends on failed to start";
    case 1069: // ERROR_SERVICE_LOGON_FAILED
        return "The service couldn't log on as its account";
    case 1072: // ERROR_SERVICE_MARKED_FOR_DELETE
        return "The service is marked for deletion";
    case 1115: // ERROR_SHUTDOWN_IN_PROGRESS
        return "Windows is shutting down";
    default:
        return std::format("Windows error {}", code);
    }
}

/// The message for a stop refused because these services, still running, depend on it.
[[nodiscard]] inline std::string dependentsText(std::span<const std::string> dependents)
{
    if (dependents.empty())
    {
        return errorText(1051);
    }
    std::string text = "Stop the services that depend on it first: ";
    for (std::size_t i = 0; i < dependents.size(); ++i)
    {
        text += (i == 0) ? "" : ", ";
        text += dependents[i];
    }
    return text;
}

/// The SCM's dwStartType for a start type, and whether delayed auto-start is set with it.
struct StartTypeCode
{
    std::uint32_t code = 0;
    bool delayed = false;
};

/// SERVICE_AUTO_START (2), SERVICE_DEMAND_START (3), SERVICE_DISABLED (4); nullopt for a start type
/// that cannot be set from here (Boot, System and Unknown apply only to drivers or are no type).
[[nodiscard]] constexpr std::optional<StartTypeCode> startTypeCode(ServiceStartType startType) noexcept
{
    switch (startType)
    {
    case ServiceStartType::Automatic:
        return StartTypeCode{.code = 2, .delayed = false};
    case ServiceStartType::AutomaticDelayed:
        return StartTypeCode{.code = 2, .delayed = true};
    case ServiceStartType::Manual:
        return StartTypeCode{.code = 3, .delayed = false};
    case ServiceStartType::Disabled:
        return StartTypeCode{.code = 4, .delayed = false};
    case ServiceStartType::Unknown:
    case ServiceStartType::Boot:
    case ServiceStartType::System:
        break;
    }
    return std::nullopt;
}

} // namespace Platform::Windows::ServiceActionMath
