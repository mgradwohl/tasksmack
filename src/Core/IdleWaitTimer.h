#pragma once

// The frame loop's precise idle wake on Windows (#3712).
//
// SDL raises the system timer to 1 ms (timeBeginPeriod(1)) unless SDL_HINT_TIMER_RESOLUTION says
// otherwise, and the loop's idle wait relied on it: SDL_WaitEventTimeout() waits with
// MsgWaitForMultipleObjects() in whole milliseconds, which ends on a system timer tick. At Windows'
// default 15.625 ms tick, the 50 ms idle period became 62.5 ms (4 ticks), 20 fps instead of 22.
// Application sets the hint to 0, so TaskSmack no longer holds the whole machine's timer at 1 ms, and
// this ends each wait on time instead: a high-resolution waitable timer
// (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, as SDL_DelayPrecise() uses), due when the wait's timeout
// runs out, whose signal a thread-pool wait turns into a WM_NULL posted to the loop thread. That
// message wakes SDL's MsgWaitForMultipleObjects(); SDL pumps it, finds no event, sees its timeout
// elapsed and returns. The wait is still SDL's own, so everything that woke it before still does:
// input, window messages, and an SDL_PushEvent() from another thread (SDL's wakeup message).
//
// A WM_NULL that arrives after the wait has ended (an event woke it first, and the timer fired before
// disarm()) stays queued until the next wait's pump takes it, which dispatches nothing.
//
// The Win32 calls go through IdleWaitTimerFunctions, so arming and disarming are tested with fakes
// (tests/Core/test_IdleWaitTimer.cpp). Loop thread only, apart from the thread-pool callback, which
// only posts the message.

#include <cstdint>
#include <optional>

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
#endif

namespace Core
{

/// SetWaitableTimerEx()'s due time for a wait of @p timeoutMs from now: relative, so negative, in
/// 100-ns units. None for a wait that needs no timer: 0 (a poll) or a negative timeout (no timeout).
[[nodiscard]] constexpr auto idleWaitTimerDueTime(int timeoutMs) noexcept -> std::optional<std::int64_t>
{
    if (timeoutMs <= 0)
    {
        return std::nullopt;
    }
    constexpr std::int64_t HUNDRED_NS_PER_MS = 10'000;
    return -(static_cast<std::int64_t>(timeoutMs) * HUNDRED_NS_PER_MS);
}

#ifdef _WIN32

/// The Win32 calls IdleWaitTimer makes: the system's by default (systemFunctions()), fakes in tests.
struct IdleWaitTimerFunctions
{
    HANDLE(WINAPI* createWaitableTimerExW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) = nullptr;
    BOOL(WINAPI* setWaitableTimerEx)(HANDLE, const LARGE_INTEGER*, LONG, PTIMERAPCROUTINE, LPVOID, PREASON_CONTEXT, ULONG) = nullptr;
    BOOL(WINAPI* cancelWaitableTimer)(HANDLE) = nullptr;
    BOOL(WINAPI* closeHandle)(HANDLE) = nullptr;
    PTP_WAIT(WINAPI* createThreadpoolWait)(PTP_WAIT_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON) = nullptr;
    VOID(WINAPI* setThreadpoolWait)(PTP_WAIT, HANDLE, PFILETIME) = nullptr;
    VOID(WINAPI* waitForThreadpoolWaitCallbacks)(PTP_WAIT, BOOL) = nullptr;
    VOID(WINAPI* closeThreadpoolWait)(PTP_WAIT) = nullptr;
    BOOL(WINAPI* postThreadMessageW)(DWORD, UINT, WPARAM, LPARAM) = nullptr;

    [[nodiscard]] static IdleWaitTimerFunctions systemFunctions() noexcept
    {
        return {
            .createWaitableTimerExW = &CreateWaitableTimerExW,
            .setWaitableTimerEx = &SetWaitableTimerEx,
            .cancelWaitableTimer = &CancelWaitableTimer,
            .closeHandle = &CloseHandle,
            .createThreadpoolWait = &CreateThreadpoolWait,
            .setThreadpoolWait = &SetThreadpoolWait,
            .waitForThreadpoolWaitCallbacks = &WaitForThreadpoolWaitCallbacks,
            .closeThreadpoolWait = &CloseThreadpoolWait,
            .postThreadMessageW = &PostThreadMessageW,
        };
    }
};

/// Ends the loop thread's event waits on time while the system timer runs at its default tick (#3712).
/// Built on the loop thread, which it wakes. Without a high-resolution timer (before Windows 10 1803,
/// or a failed call) it does nothing, and the waits end on the tick as SDL's own would.
class IdleWaitTimer
{
  public:
    explicit IdleWaitTimer(const IdleWaitTimerFunctions& fns = IdleWaitTimerFunctions::systemFunctions()) noexcept
        : m_Fns(fns),
          m_LoopThreadId(GetCurrentThreadId()),
          m_Timer(m_Fns.createWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    {
        if (m_Timer == nullptr)
        {
            return;
        }
        m_Wait = m_Fns.createThreadpoolWait(&IdleWaitTimer::onTimerSignalled, this, nullptr);
        if (m_Wait == nullptr)
        {
            m_Fns.closeHandle(m_Timer);
            m_Timer = nullptr;
        }
    }

    ~IdleWaitTimer()
    {
        if (m_Wait != nullptr)
        {
            m_Fns.setThreadpoolWait(m_Wait, nullptr, nullptr);
            m_Fns.waitForThreadpoolWaitCallbacks(m_Wait, TRUE);
            m_Fns.closeThreadpoolWait(m_Wait);
        }
        if (m_Timer != nullptr)
        {
            m_Fns.closeHandle(m_Timer);
        }
    }

    IdleWaitTimer(const IdleWaitTimer&) = delete;
    IdleWaitTimer& operator=(const IdleWaitTimer&) = delete;
    IdleWaitTimer(IdleWaitTimer&&) = delete; // the thread-pool wait holds this object's address
    IdleWaitTimer& operator=(IdleWaitTimer&&) = delete;

    /// Whether the timer and its thread-pool wait were made: without them arm() does nothing.
    [[nodiscard]] bool active() const noexcept
    {
        return m_Wait != nullptr;
    }

    /// Before an event wait of @p timeoutMs (SDL_WaitEventTimeout()): wake the loop thread when it runs
    /// out. A poll (0) or a wait with no timeout (negative) needs no wake.
    void arm(int timeoutMs) noexcept
    {
        const std::optional<std::int64_t> due = idleWaitTimerDueTime(timeoutMs);
        if (!active() || !due.has_value())
        {
            return;
        }
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = *due;
        // The thread-pool wait is one-shot: registered again for each wait, before the timer is set.
        m_Fns.setThreadpoolWait(m_Wait, m_Timer, nullptr);
        if (m_Fns.setWaitableTimerEx(m_Timer, &dueTime, 0, nullptr, nullptr, nullptr, 0) == FALSE)
        {
            m_Fns.setThreadpoolWait(m_Wait, nullptr, nullptr);
            return;
        }
        m_Armed = true;
    }

    /// After the wait, however it ended: a timer still pending wakes nothing.
    void disarm() noexcept
    {
        if (!m_Armed)
        {
            return;
        }
        m_Armed = false;
        m_Fns.cancelWaitableTimer(m_Timer);
        m_Fns.setThreadpoolWait(m_Wait, nullptr, nullptr);
    }

  private:
    // On a thread-pool thread: the timer is due, so wake the loop thread's message wait.
    static VOID CALLBACK onTimerSignalled(PTP_CALLBACK_INSTANCE /*instance*/,
                                          PVOID context,
                                          PTP_WAIT /*wait*/,
                                          TP_WAIT_RESULT /*result*/) noexcept
    {
        const auto* self = static_cast<const IdleWaitTimer*>(context);
        self->m_Fns.postThreadMessageW(self->m_LoopThreadId, WM_NULL, 0, 0);
    }

    IdleWaitTimerFunctions m_Fns;
    DWORD m_LoopThreadId = 0;
    HANDLE m_Timer = nullptr;
    PTP_WAIT m_Wait = nullptr;
    bool m_Armed = false;
};

#endif // _WIN32

} // namespace Core
