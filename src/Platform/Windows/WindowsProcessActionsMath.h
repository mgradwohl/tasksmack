#pragma once

#ifdef _WIN32

#include "Domain/PriorityConfig.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstdint>
#include <format>
#include <string>

namespace Platform
{

/// Maps a Unix-style nice value (-20 to 19) to the closest Windows priority class, using
/// the same threshold boundaries as Domain::Priority::getPriorityLabel. Extracted as a free
/// function so its five threshold branches can be unit tested directly, without spawning or
/// opening a real process. We intentionally never return REALTIME_PRIORITY_CLASS, to avoid
/// system instability.
[[nodiscard]] inline uint32_t niceToPriorityClass(int32_t nice)
{
    if (nice < Domain::Priority::HIGH_THRESHOLD)
    {
        return HIGH_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::ABOVE_NORMAL_THRESHOLD)
    {
        return ABOVE_NORMAL_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::BELOW_NORMAL_THRESHOLD)
    {
        return NORMAL_PRIORITY_CLASS;
    }
    if (nice < Domain::Priority::IDLE_THRESHOLD)
    {
        return BELOW_NORMAL_PRIORITY_CLASS;
    }
    return IDLE_PRIORITY_CLASS;
}

/// Whether Terminate's close request goes to this top-level window (#1094): one the target process
/// owns, that is visible, and that has no owner window. Those are the windows a user would close:
/// closing an owned window (a dialog, a tool window) or an invisible helper window does not ask the
/// application to exit, and can confuse it.
[[nodiscard]] constexpr bool isCloseRequestWindow(uint32_t windowPid, uint32_t targetPid, bool visible, bool hasOwner) noexcept
{
    return windowPid == targetPid && visible && !hasOwner;
}

/// Terminate's result once its close requests have been posted (#1094). Windows has no SIGTERM:
/// a graceful exit is asked for by closing the process's windows.
///
/// @param eligible    Windows that should be asked (see isCloseRequestWindow).
/// @param failed      Of those, how many the close request could not be queued for -- PostMessage
///                    can be refused, e.g. by UIPI when the target runs elevated.
/// @param firstError  GetLastError() for the first refused request.
/// @return Empty when every eligible window was asked; otherwise why the process was not fully
///         asked. No window at all (a service, a console or background process) means it cannot be
///         asked, only ended with Kill.
[[nodiscard]] inline std::string closeRequestFailure(int32_t pid, int eligible, int failed, uint32_t firstError)
{
    if (eligible <= 0)
    {
        return std::format("Process {} has no window to close, so it cannot be asked to exit; use Kill to end it", pid);
    }
    if (failed <= 0)
    {
        return {};
    }
    if (failed >= eligible)
    {
        return std::format("Could not ask process {} to close: error {} (it may be running elevated); use Kill to end it", pid, firstError);
    }
    return std::format("Asked {} of {} windows of process {} to close; the rest refused the request (error {})",
                       eligible - failed,
                       eligible,
                       pid,
                       firstError);
}

} // namespace Platform

#endif // _WIN32
