#pragma once

// Per-process UDP bytes on Windows from an ETW real-time session (#1492).
//
// Windows has no user-mode per-process UDP byte counter: TCP EStats is TCP only, GetExtendedUdpTable
// gives owners without bytes, and PDH and GetIfEntry2 are per adapter (#1258). The kernel's network
// provider, Microsoft-Windows-Kernel-Network, does report every UDP datagram with its process and
// size. This collector starts a private real-time session on it, named for this process
// (EtwNet::sessionName()), with only its UDP send and receive events enabled (IDs 42/43, IPv4, and
// 58/59, IPv6), and consumes it on a thread of its own whose callback adds each datagram's bytes to
// its process's counters (EtwNet::UdpByteCounters).
//
// Opt-in: App starts it only when the Settings toggle is on, and it starts only when TaskSmack runs
// elevated (a session on a kernel provider needs administrator rights; the Performance Log Users
// group is deliberately not used). Before starting, it stops any TaskSmack session a crashed
// instance left (named for a process that is gone, or whose PID now belongs to another process),
// but never one another running TaskSmack owns. It stops its own session on stop() and on
// destruction.
//
// Every ETW call goes through a table of function pointers (Functions), the system's by default, so
// tests drive start, stop, the stale-session sweep and the callback with fabricated EVENT_RECORDs
// (tests/Platform/test_WindowsEtwNetCollector.cpp).

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers; their symbols come from sub-headers
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/Windows/WindowsEtwNetMath.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Platform
{

class WindowsEtwNetCollector final : public IUdpTrafficCollector
{
  public:
    /// The calls the collector makes: the ETW controller and consumer APIs (advapi32), and the
    /// process checks the stale-session sweep and the elevation gate need. Every member must be set.
    struct Functions
    {
        // The SDK names the handles CONTROLTRACE_ID and PROCESSTRACE_HANDLE (older SDKs:
        // TRACEHANDLE); all are ULONG64.
        ULONG(WINAPI* startTraceW)(ULONG64* traceId, LPCWSTR name, PEVENT_TRACE_PROPERTIES properties) = nullptr;
        ULONG(WINAPI* controlTraceW)(ULONG64 traceId, LPCWSTR name, PEVENT_TRACE_PROPERTIES properties, ULONG controlCode) = nullptr;
        ULONG(WINAPI* enableTraceEx2)
        (ULONG64 traceId,
         LPCGUID provider,
         ULONG controlCode,
         UCHAR level,
         ULONGLONG matchAnyKeyword,
         ULONGLONG matchAllKeyword,
         ULONG timeout,
         PENABLE_TRACE_PARAMETERS parameters) = nullptr;
        ULONG64(WINAPI* openTraceW)(PEVENT_TRACE_LOGFILEW logfile) = nullptr;
        ULONG(WINAPI* processTrace)(ULONG64* handles, ULONG handleCount, LPFILETIME start, LPFILETIME end) = nullptr;
        ULONG(WINAPI* closeTrace)(ULONG64 handle) = nullptr;
        ULONG(WINAPI* queryAllTracesW)(PEVENT_TRACE_PROPERTIES* properties, ULONG count, PULONG loggerCount) = nullptr;

        bool (*isElevated)() = nullptr;
        /// This process: its PID and creation time, which name its session.
        EtwNet::SessionOwner (*self)() = nullptr;
        /// Whether a session's owner still runs (the stale-session sweep).
        EtwNet::OwnerLiveness (*ownerLiveness)(const EtwNet::SessionOwner& owner) = nullptr;
    };

    /// The session's buffers, sized for heavy UDP (QUIC at line rate is ~100k events/s): 64 KB
    /// buffers, up to 64 of them (4 MB at most, allocated only as needed), flushed every second.
    static constexpr ULONG BUFFER_SIZE_KB = 64;
    static constexpr ULONG MINIMUM_BUFFERS = 8;
    static constexpr ULONG MAXIMUM_BUFFERS = 64;
    static constexpr ULONG FLUSH_TIMER_SECONDS = 1;

    /// Sessions the stale sweep reads at most (ETW's own default limit is 64).
    static constexpr ULONG MAX_SESSIONS_QUERIED = 64;

    WindowsEtwNetCollector();
    explicit WindowsEtwNetCollector(const Functions& functions);
    ~WindowsEtwNetCollector() override;

    WindowsEtwNetCollector(const WindowsEtwNetCollector&) = delete;
    WindowsEtwNetCollector& operator=(const WindowsEtwNetCollector&) = delete;
    WindowsEtwNetCollector(WindowsEtwNetCollector&&) = delete;
    WindowsEtwNetCollector& operator=(WindowsEtwNetCollector&&) = delete;

    /// The real calls.
    [[nodiscard]] static Functions systemFunctions();

    [[nodiscard]] UdpCollectorStartResult start() override;
    void stop() override;
    [[nodiscard]] bool isDelivering() const override;
    [[nodiscard]] std::vector<UdpProcessTraffic> sample() override;

    /// The consumer's per-event callback (EVENT_TRACE_LOGFILEW::EventRecordCallback). @p record's
    /// UserContext is the collector. Public so tests can feed it fabricated records.
    static void WINAPI onEventRecord(PEVENT_RECORD record) noexcept;

    /// Add one event to the counters, if it is a UDP event of the kernel network provider.
    void handleEvent(const EVENT_RECORD& record) noexcept;

    /// The session's name while it runs (empty otherwise). For tests and logs.
    [[nodiscard]] std::wstring sessionName() const;

    /// Events seen that were not UDP events of the provider, or were malformed (ignored).
    [[nodiscard]] std::uint64_t ignoredEvents() const noexcept
    {
        return m_IgnoredEvents.load(std::memory_order_relaxed);
    }

  private:
    /// EVENT_TRACE_PROPERTIES with room for the session name, and for a log file name when the
    /// sweep queries sessions (QueryAllTracesW fills both).
    struct SessionProperties
    {
        EVENT_TRACE_PROPERTIES properties;
        std::array<wchar_t, 1024> loggerName;
        std::array<wchar_t, 1024> logFileName;
    };

    [[nodiscard]] static SessionProperties makeProperties();
    void stopStaleSessions();
    void stopSessionByName(const std::wstring& name) const;
    void stopLocked();
    void consume(ULONG64 consumerHandle);
    void logLostEvents();

    Functions m_Fns;
    mutable std::mutex m_ControlMutex; // start(), stop(), the lost-events query
    std::wstring m_SessionName;        // Guarded by m_ControlMutex; empty when stopped
    ULONG64 m_SessionId = 0;           // Guarded by m_ControlMutex
    ULONG64 m_ConsumerHandle = 0;      // Guarded by m_ControlMutex
    std::jthread m_Consumer;           // Started and joined under m_ControlMutex

    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Delivering{false};
    std::atomic<std::uint64_t> m_Events{0};
    std::atomic<std::uint64_t> m_IgnoredEvents{0};
    std::uint64_t m_LastEventsLost = 0;      // Guarded by m_ControlMutex
    std::uint64_t m_LastBuffersLost = 0;     // Guarded by m_ControlMutex
    std::atomic<std::uint64_t> m_Samples{0}; // sample() calls, to log the lost events every ~minute

    std::mutex m_CountersMutex;
    EtwNet::UdpByteCounters m_Counters; // Guarded by m_CountersMutex
};

} // namespace Platform
