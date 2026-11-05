#include "WindowsEtwNetCollector.h"

#include "Domain/Worker.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ThreadName.h"
#include "Platform/Windows/WindowsConversions.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsEtwNetMath.h"
#include "Platform/Windows/WindowsHandles.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

namespace
{

// Microsoft-Windows-Kernel-Network.
constexpr GUID KERNEL_NETWORK_PROVIDER = {
    .Data1 = 0x7DD42A49,
    .Data2 = 0x5329,
    .Data3 = 0x4832,
    .Data4 = {0x8D, 0xFD, 0x43, 0xD9, 0x79, 0x15, 0x3A, 0x88},
};

// What OpenTraceW returns on failure: INVALID_PROCESSTRACE_HANDLE, (TRACEHANDLE)INVALID_HANDLE_VALUE,
// spelled without the macro's C-style cast. TaskSmack is 64-bit only.
static_assert(sizeof(void*) == 8, "INVALID_CONSUMER_HANDLE assumes a 64-bit process");
constexpr ULONG64 INVALID_CONSUMER_HANDLE = ~ULONG64{0};

// QPC timestamps (EVENT_TRACE_PROPERTIES::Wnode.ClientContext): the cheapest clock; the collector
// never reads the timestamps anyway.
constexpr ULONG CLIENT_CONTEXT_QPC = 1;

/// The ASCII text of a session name QueryAllTracesW returned, or nullopt for any other character:
/// TaskSmack's names are ASCII, so a name that isn't is not one of them.
[[nodiscard]] std::optional<std::string> asciiName(std::wstring_view name)
{
    std::string ascii;
    ascii.reserve(name.size());
    std::size_t index = 0;
    while (index < name.size())
    {
        const wchar_t c = name[index];
        if (c <= 0 || c >= 0x80)
        {
            return std::nullopt;
        }
        ascii.push_back(static_cast<char>(c));
        ++index;
    }
    return ascii;
}

[[nodiscard]] std::wstring widen(std::string_view ascii)
{
    std::wstring wide;
    wide.reserve(ascii.size());
    for (const char c : ascii)
    {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return wide;
}

[[nodiscard]] bool systemIsElevated()
{
    return Windows::isProcessElevated();
}

[[nodiscard]] EtwNet::SessionOwner systemSelf()
{
    EtwNet::SessionOwner self{.pid = GetCurrentProcessId()};
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user) != FALSE)
    {
        self.creationTime = Windows::fileTimeTicks(creation);
    }
    return self;
}

[[nodiscard]] EtwNet::OwnerLiveness systemOwnerLiveness(const EtwNet::SessionOwner& owner)
{
    const Windows::UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid));
    if (!process)
    {
        // ERROR_INVALID_PARAMETER: no process has that PID. Anything else (access denied to a
        // protected process) leaves it unknown, and the session is left alone.
        return GetLastError() == ERROR_INVALID_PARAMETER ? EtwNet::OwnerLiveness::Gone : EtwNet::OwnerLiveness::Unknown;
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.get(), &exitCode) != FALSE && exitCode != STILL_ACTIVE)
    {
        return EtwNet::OwnerLiveness::Gone; // Exited; something still holds a handle to it
    }
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process.get(), &creation, &exitTime, &kernel, &user) == FALSE)
    {
        return EtwNet::OwnerLiveness::Unknown;
    }
    return Windows::fileTimeTicks(creation) == owner.creationTime ? EtwNet::OwnerLiveness::Alive : EtwNet::OwnerLiveness::PidReused;
}

[[nodiscard]] UdpCollectorStartResult startFailure(ULONG status) noexcept
{
    return status == ERROR_ACCESS_DENIED ? UdpCollectorStartResult::Denied : UdpCollectorStartResult::Failed;
}

/// The event-ID filter: only the provider's UDP events reach the session (Windows 8.1 and later).
struct UdpEventIdFilter
{
    static constexpr std::array<USHORT, 4> IDS{
        EtwNet::UDP4_SEND_EVENT_ID,
        EtwNet::UDP4_RECV_EVENT_ID,
        EtwNet::UDP6_SEND_EVENT_ID,
        EtwNet::UDP6_RECV_EVENT_ID,
    };
    static constexpr std::size_t HEADER_SIZE = offsetof(EVENT_FILTER_EVENT_ID, Events);

    alignas(EVENT_FILTER_EVENT_ID) std::array<std::byte, HEADER_SIZE + sizeof(IDS)> bytes{};

    UdpEventIdFilter()
    {
        EVENT_FILTER_EVENT_ID header{};
        header.FilterIn = TRUE; // Deliver these IDs (and nothing else)
        header.Count = static_cast<USHORT>(IDS.size());
        std::memcpy(bytes.data(), &header, HEADER_SIZE);
        std::memcpy(std::span(bytes).subspan(HEADER_SIZE).data(), IDS.data(), sizeof(IDS));
    }
};

} // namespace

WindowsEtwNetCollector::WindowsEtwNetCollector() : WindowsEtwNetCollector(systemFunctions())
{}

WindowsEtwNetCollector::WindowsEtwNetCollector(const Functions& functions) : m_Fns(functions)
{}

WindowsEtwNetCollector::~WindowsEtwNetCollector()
{
    try
    {
        stop();
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a destructor must not throw; stop() logs what it can
    {}
}

WindowsEtwNetCollector::Functions WindowsEtwNetCollector::systemFunctions()
{
    return Functions{
        .startTraceW = &StartTraceW,
        .controlTraceW = &ControlTraceW,
        .enableTraceEx2 = &EnableTraceEx2,
        .openTraceW = &OpenTraceW,
        .processTrace = &ProcessTrace,
        .closeTrace = &CloseTrace,
        .queryAllTracesW = &QueryAllTracesW,
        .isElevated = &systemIsElevated,
        .self = &systemSelf,
        .ownerLiveness = &systemOwnerLiveness,
    };
}

WindowsEtwNetCollector::SessionProperties WindowsEtwNetCollector::makeProperties()
{
    SessionProperties props{};
    EVENT_TRACE_PROPERTIES& p = props.properties;
    p.Wnode.BufferSize = sizeof(SessionProperties);
    p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p.Wnode.ClientContext = CLIENT_CONTEXT_QPC;
    p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p.BufferSize = BUFFER_SIZE_KB;
    p.MinimumBuffers = MINIMUM_BUFFERS;
    p.MaximumBuffers = MAXIMUM_BUFFERS;
    p.FlushTimer = FLUSH_TIMER_SECONDS;
    p.LoggerNameOffset = offsetof(SessionProperties, loggerName);
    p.LogFileNameOffset = offsetof(SessionProperties, logFileName);
    return props;
}

UdpCollectorStartResult WindowsEtwNetCollector::start()
{
    const std::scoped_lock lock(m_ControlMutex);
    if (m_Running.load(std::memory_order_acquire))
    {
        return UdpCollectorStartResult::Started;
    }
    // A session the consumer saw end on its own (stopped by another tool) is cleaned up first.
    stopLocked();

    if (!m_Fns.isElevated())
    {
        spdlog::info("UDP event trace: not started (TaskSmack is not running as administrator); network counters stay TCP only");
        return UdpCollectorStartResult::NotElevated;
    }

    stopStaleSessions();

    const std::string asciiSessionName = EtwNet::sessionName(m_Fns.self());
    const std::wstring name = widen(asciiSessionName);
    ULONG64 sessionId = 0;
    SessionProperties props = makeProperties();
    ULONG status = m_Fns.startTraceW(&sessionId, name.c_str(), &props.properties);
    if (status == ERROR_ALREADY_EXISTS)
    {
        // This process's own session, left by a stop that failed: replaced.
        stopSessionByName(name);
        props = makeProperties();
        status = m_Fns.startTraceW(&sessionId, name.c_str(), &props.properties);
    }
    if (status != ERROR_SUCCESS)
    {
        spdlog::warn("UDP event trace: StartTrace failed ({}); network counters stay TCP only", status);
        return startFailure(status);
    }
    m_SessionId = sessionId;
    m_SessionName = name;

    // UDP events only (IPv4 42/43, IPv6 58/59): the keywords select the address families, and the
    // event-ID filter leaves out their TCP events, which EStats already counts.
    UdpEventIdFilter filter;
    EVENT_FILTER_DESCRIPTOR filterDescriptor{};
    filterDescriptor.Ptr = reinterpret_cast<ULONGLONG>(
        filter.bytes.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - the API takes the address as an integer
    filterDescriptor.Size = static_cast<ULONG>(filter.bytes.size());
    filterDescriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS parameters{};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableFilterDesc = &filterDescriptor;
    parameters.FilterDescCount = 1;
    constexpr ULONGLONG keywords = EtwNet::KEYWORD_IPV4 | EtwNet::KEYWORD_IPV6;
    status = m_Fns.enableTraceEx2(
        sessionId, &KERNEL_NETWORK_PROVIDER, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords, 0, 0, &parameters);
    if (status == ERROR_NOT_SUPPORTED || status == ERROR_INVALID_PARAMETER)
    {
        // A system without event-ID filtering: every event of the two keywords then reaches the
        // session, and the callback drops the TCP ones (more overhead, same counts).
        spdlog::debug("UDP event trace: event-ID filter refused ({}); filtering in the callback", status);
        status = m_Fns.enableTraceEx2(
            sessionId, &KERNEL_NETWORK_PROVIDER, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords, 0, 0, nullptr);
    }
    if (status != ERROR_SUCCESS)
    {
        spdlog::warn("UDP event trace: enabling Microsoft-Windows-Kernel-Network failed ({}); network counters stay TCP only", status);
        stopLocked();
        return startFailure(status);
    }

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = m_SessionName.data();
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - EVENT_TRACE_LOGFILEW's own layout
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = &WindowsEtwNetCollector::onEventRecord;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    logfile.Context = this;
    const ULONG64 consumer = m_Fns.openTraceW(&logfile);
    if (consumer == INVALID_CONSUMER_HANDLE)
    {
        spdlog::warn("UDP event trace: OpenTrace failed ({}); network counters stay TCP only", GetLastError());
        stopLocked();
        return UdpCollectorStartResult::Failed;
    }
    m_ConsumerHandle = consumer;

    {
        const std::scoped_lock countersLock(m_CountersMutex);
        m_Counters.clear();
    }
    m_Delivering.store(false, std::memory_order_release);
    m_Events.store(0, std::memory_order_relaxed);
    m_IgnoredEvents.store(0, std::memory_order_relaxed);
    m_LastEventsLost = 0;
    m_LastBuffersLost = 0;
    m_Running.store(true, std::memory_order_release);
    if (!startConsumerLocked(consumer))
    {
        stopLocked();
        return UdpCollectorStartResult::Failed;
    }
    spdlog::info("UDP event trace: session {} started; per-process network counters include UDP once it delivers", asciiSessionName);
    return UdpCollectorStartResult::Started;
}

void WindowsEtwNetCollector::stop()
{
    const std::scoped_lock lock(m_ControlMutex);
    stopLocked();
}

void WindowsEtwNetCollector::stopLocked()
{
    if (!m_SessionName.empty())
    {
        // EVENT_TRACE_CONTROL_STOP returns the session's final statistics.
        SessionProperties props = makeProperties();
        const ULONG status = m_Fns.controlTraceW(m_SessionId, nullptr, &props.properties, EVENT_TRACE_CONTROL_STOP);
        if (status == ERROR_SUCCESS)
        {
            spdlog::debug("UDP event trace: stopped after {} UDP events; {} events lost, {} real-time buffers lost",
                          m_Events.load(std::memory_order_relaxed),
                          props.properties.EventsLost,
                          props.properties.RealTimeBuffersLost);
        }
        else if (status != ERROR_WMI_INSTANCE_NOT_FOUND)
        {
            spdlog::warn("UDP event trace: stopping the session failed ({})", status);
        }
    }
    // The session's end makes ProcessTrace return; CloseTrace makes it return even if the session
    // could not be stopped, and frees the consumer handle either way.
    if (m_ConsumerHandle != 0)
    {
        (void) m_Fns.closeTrace(m_ConsumerHandle);
        m_ConsumerHandle = 0;
    }
    // Destroying the worker stops it and joins its thread, which ProcessTrace's return lets go.
    m_Consumer.reset();
    m_Running.store(false, std::memory_order_release);
    m_Delivering.store(false, std::memory_order_release);
    m_SessionName.clear();
    m_SessionId = 0;
    const std::scoped_lock countersLock(m_CountersMutex);
    m_Counters.clear();
}

bool WindowsEtwNetCollector::startConsumerLocked(ULONG64 consumerHandle)
{
    // A worker of its own per session: its one job runs until the session ends, and a worker that has
    // ended (Stopped, or Failed) takes no more work. Its thread starts with the submit, as the jthread did.
    try
    {
        m_Consumer.emplace(
            ConsumerWorker::Options{.threadName = UDP_TRACE_THREAD_NAME, .capacity = 1, .admission = Domain::Admission::Refuse},
            [this](ConsumeSession& session, const std::stop_token& /*stop*/, ConsumerWorker::JobContext& /*context*/)
            { consume(session.consumerHandle); });
    }
    catch (const std::exception& e) // no memory for the worker's state
    {
        spdlog::warn("UDP event trace: could not start its consumer thread ({})", e.what());
        return false;
    }
    const Domain::SubmitResult submitted = m_Consumer->submit(ConsumeSession{.consumerHandle = consumerHandle});
    if (submitted == Domain::SubmitResult::Queued)
    {
        return true;
    }
    // Closed: no thread could be started, and the worker is Failed. Refused: no memory to queue the job.
    // Either way nothing consumes the session, and the caller stops it; a later start() tries again.
    // The reason is read before it is logged; a copy of it that fails (no memory) reads as a fixed text.
    std::string failure;
    std::string_view reason = "no memory to queue its job";
    if (m_Consumer->state() == Domain::WorkerState::Failed)
    {
        try
        {
            failure = m_Consumer->failure();
            reason = failure;
        }
        catch (const std::exception&)
        {
            reason = ConsumerWorker::FAILURE_UNRECORDED;
        }
    }
    spdlog::warn("UDP event trace: could not start its consumer thread ({})", reason);
    return false;
}

void WindowsEtwNetCollector::consume(ULONG64 consumerHandle)
{
    // Nothing thrown here leaves the job (#1695): a failure becomes a status and a log line. The worker's
    // own boundary (#3343) is the backstop, and names the thread (UDP_TRACE_THREAD_NAME).
    ULONG status = ERROR_SUCCESS;
    try
    {
        ULONG64 handle = consumerHandle;
        status = m_Fns.processTrace(&handle, 1, nullptr, nullptr);
    }
    catch (...)
    {
        status = ERROR_UNHANDLED_EXCEPTION;
    }
    // The session ended: stopped by stop(), or from outside (logman stop, a reboot of the provider).
    // Either way the counters no longer include UDP.
    m_Delivering.store(false, std::memory_order_release);
    m_Running.store(false, std::memory_order_release);
    if (status != ERROR_SUCCESS && status != ERROR_CANCELLED)
    {
        try
        {
            spdlog::warn("UDP event trace: the consumer stopped ({}); network counters are TCP only again", status);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - logging is best effort on the way out of the thread
        {}
    }
}

void WINAPI WindowsEtwNetCollector::onEventRecord(PEVENT_RECORD record) noexcept
{
    if (record == nullptr || record->UserContext == nullptr)
    {
        return;
    }
    static_cast<WindowsEtwNetCollector*>(record->UserContext)->handleEvent(*record);
}

void WindowsEtwNetCollector::handleEvent(const EVENT_RECORD& record) noexcept
{
    if (IsEqualGUID(record.EventHeader.ProviderId, KERNEL_NETWORK_PROVIDER) == FALSE)
    {
        m_IgnoredEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::span<const std::byte> payload = (record.UserData != nullptr)
                                                 ? std::span(static_cast<const std::byte*>(record.UserData), record.UserDataLength)
                                                 : std::span<const std::byte>{};
    const std::optional<EtwNet::UdpPacket> packet = EtwNet::parseUdpEvent(record.EventHeader.EventDescriptor.Id, payload);
    if (!packet.has_value())
    {
        m_IgnoredEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try
    {
        const std::scoped_lock lock(m_CountersMutex);
        m_Counters.add(*packet);
    }
    catch (...)
    {
        // A new process's entry couldn't be allocated: this datagram is not counted.
        m_IgnoredEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_Events.fetch_add(1, std::memory_order_relaxed);
    m_Delivering.store(true, std::memory_order_release);
}

bool WindowsEtwNetCollector::isDelivering() const
{
    return m_Running.load(std::memory_order_acquire) && m_Delivering.load(std::memory_order_acquire);
}

std::vector<UdpProcessTraffic> WindowsEtwNetCollector::sample()
{
    if (!m_Running.load(std::memory_order_acquire))
    {
        return {};
    }
    std::vector<UdpProcessTraffic> totals;
    {
        const std::scoped_lock lock(m_CountersMutex);
        totals = m_Counters.sample();
    }
    // About once a minute at the default interval: how many events the session lost (#1492). Skipped
    // when start() or stop() holds the control lock; the next minute's line has the totals.
    constexpr std::uint64_t LOG_EVERY_SAMPLES = 60;
    if (m_Samples.fetch_add(1, std::memory_order_relaxed) % LOG_EVERY_SAMPLES == 0)
    {
        const std::unique_lock lock(m_ControlMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            logLostEvents();
        }
    }
    return totals;
}

void WindowsEtwNetCollector::logLostEvents()
{
    if (m_SessionName.empty())
    {
        return;
    }
    SessionProperties props = makeProperties();
    if (m_Fns.controlTraceW(m_SessionId, nullptr, &props.properties, EVENT_TRACE_CONTROL_QUERY) != ERROR_SUCCESS)
    {
        return;
    }
    const std::uint64_t eventsLost = props.properties.EventsLost;
    const std::uint64_t buffersLost = props.properties.RealTimeBuffersLost;
    spdlog::debug("UDP event trace: {} UDP events, {} ignored; {} events lost (+{}), {} real-time buffers lost (+{})",
                  m_Events.load(std::memory_order_relaxed),
                  m_IgnoredEvents.load(std::memory_order_relaxed),
                  eventsLost,
                  eventsLost - std::min(eventsLost, m_LastEventsLost),
                  buffersLost,
                  buffersLost - std::min(buffersLost, m_LastBuffersLost));
    m_LastEventsLost = eventsLost;
    m_LastBuffersLost = buffersLost;
}

void WindowsEtwNetCollector::stopStaleSessions()
{
    // Every running session, each with room for its names. A failed query sweeps nothing: a stale
    // session then only costs a session slot and is tried again at the next start.
    std::vector<SessionProperties> sessions(MAX_SESSIONS_QUERIED, makeProperties());
    std::vector<PEVENT_TRACE_PROPERTIES> pointers;
    pointers.reserve(sessions.size());
    for (SessionProperties& session : sessions)
    {
        pointers.push_back(&session.properties);
    }
    ULONG count = 0;
    const ULONG status = m_Fns.queryAllTracesW(pointers.data(), static_cast<ULONG>(pointers.size()), &count);
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA)
    {
        spdlog::debug("UDP event trace: listing sessions failed ({}); no stale session stopped", status);
        return;
    }
    const EtwNet::SessionOwner self = m_Fns.self();
    std::size_t index = 0;
    while (index < std::min<std::size_t>(count, sessions.size()))
    {
        const SessionProperties& session = sessions[index];
        ++index;
        const std::wstring_view wideName(session.loggerName.data(), wcsnlen(session.loggerName.data(), session.loggerName.size()));
        const std::optional<std::string> name = asciiName(wideName);
        if (!name.has_value())
        {
            continue;
        }
        const std::optional<EtwNet::SessionOwner> owner = EtwNet::parseSessionName(*name);
        if (!owner.has_value())
        {
            continue;
        }
        const EtwNet::OwnerLiveness liveness = (*owner == self) ? EtwNet::OwnerLiveness::Alive : m_Fns.ownerLiveness(*owner);
        if (!EtwNet::shouldStopSession(*owner, self, liveness))
        {
            spdlog::debug("UDP event trace: leaving session {} (its TaskSmack is running, or couldn't be checked)", *name);
            continue;
        }
        spdlog::info("UDP event trace: stopping session {}, left by a TaskSmack that is no longer running", *name);
        stopSessionByName(std::wstring(wideName));
    }
}

void WindowsEtwNetCollector::stopSessionByName(const std::wstring& name) const
{
    SessionProperties props = makeProperties();
    const ULONG status = m_Fns.controlTraceW(0, name.c_str(), &props.properties, EVENT_TRACE_CONTROL_STOP);
    if (status != ERROR_SUCCESS && status != ERROR_WMI_INSTANCE_NOT_FOUND)
    {
        spdlog::debug("UDP event trace: stopping a session by name failed ({})", status);
    }
}

std::wstring WindowsEtwNetCollector::sessionName() const
{
    const std::scoped_lock lock(m_ControlMutex);
    return m_SessionName;
}

} // namespace Platform
