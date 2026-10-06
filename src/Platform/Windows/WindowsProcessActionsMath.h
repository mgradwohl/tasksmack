#pragma once

#ifdef _WIN32

#include "Domain/PriorityConfig.h"
#include "Platform/ProcessTypes.h"

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

/// Nice values the probe reports for each Windows priority class (#1204). Each sits in the middle
/// of the class's Domain::Priority label bucket, never on a threshold: -5 and -10 used to stand for
/// Above normal and High, but those are where getPriorityLabel() and niceToPriorityClass() start the
/// next class down, so Above normal was shown as Normal and High as Above Normal. Realtime has no
/// bucket of its own; MIN_NICE keeps it distinct from High (the UI names it from that value) and
/// still labels and sets it as High.
inline constexpr int32_t IDLE_CLASS_NICE = Domain::Priority::MAX_NICE;
inline constexpr int32_t BELOW_NORMAL_CLASS_NICE = 10;
inline constexpr int32_t NORMAL_CLASS_NICE = Domain::Priority::NORMAL_NICE;
inline constexpr int32_t ABOVE_NORMAL_CLASS_NICE = -7;
inline constexpr int32_t HIGH_CLASS_NICE = -15;
inline constexpr int32_t REALTIME_CLASS_NICE = Domain::Priority::MIN_NICE;

/// The nice value to report for a GetPriorityClass() result: the inverse of niceToPriorityClass()
/// for the five classes it sets (#1204). An unknown class (or GetPriorityClass() failing with 0) is
/// reported as Normal.
[[nodiscard]] constexpr int32_t priorityClassToNice(uint32_t priorityClass) noexcept
{
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:
        return IDLE_CLASS_NICE;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return BELOW_NORMAL_CLASS_NICE;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return ABOVE_NORMAL_CLASS_NICE;
    case HIGH_PRIORITY_CLASS:
        return HIGH_CLASS_NICE;
    case REALTIME_PRIORITY_CLASS:
        return REALTIME_CLASS_NICE;
    case NORMAL_PRIORITY_CLASS:
    default:
        return NORMAL_CLASS_NICE;
    }
}

/// The class a GetPriorityClass() result names (#1280), reported beside its nice value so the UI can
/// tell Realtime from High, which share a nice bucket. An unknown class (or GetPriorityClass()
/// failing with 0) is None: the nice value, Normal, then names it.
[[nodiscard]] constexpr PriorityClass toPriorityClass(uint32_t priorityClass) noexcept
{
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:
        return PriorityClass::Idle;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return PriorityClass::BelowNormal;
    case NORMAL_PRIORITY_CLASS:
        return PriorityClass::Normal;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return PriorityClass::AboveNormal;
    case HIGH_PRIORITY_CLASS:
        return PriorityClass::High;
    case REALTIME_PRIORITY_CLASS:
        return PriorityClass::Realtime;
    default:
        return PriorityClass::None;
    }
}

/// Whether Terminate's close request goes to this top-level window (#1094): one the target process
/// owns, that is visible, has no owner window, and is not a tool window. Those are the application
/// windows a user would close. Closing an owned window (a dialog), a tool window (a palette or helper,
/// owned or not: WS_EX_TOOLWINDOW is independent of ownership) or an invisible window does not ask the
/// application to exit, and can confuse it.
[[nodiscard]] constexpr bool
isCloseRequestWindow(uint32_t windowPid, uint32_t targetPid, bool visible, bool hasOwner, bool isToolWindow) noexcept
{
    return windowPid == targetPid && visible && !hasOwner && !isToolWindow;
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
