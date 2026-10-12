/// @file test_WindowsEtwNetCollector.cpp
/// @brief WindowsEtwNetCollector (#1492) against fake ETW calls: not elevated, nothing is tried; a
/// denied or failed StartTrace, EnableTraceEx2 or OpenTrace leaves it stopped (and stops what it
/// started); a started session is named for this process, enables only the UDP event IDs of
/// Microsoft-Windows-Kernel-Network under the IPv4/IPv6 keywords, and is consumed on a thread whose
/// callback sums fabricated EVENT_RECORDs per process (TCP, other providers and malformed events
/// ignored); stop() and the destructor stop the session, close the consumer and join its thread; a
/// session that ends from outside stops the delivery; the stale-session sweep stops a crashed
/// TaskSmack's session but not a running one's or another tool's. No real trace session is started.

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/Windows/WindowsEtwNetCollector.h"
#include "Platform/Windows/WindowsEtwNetMath.h"

#include <gtest/gtest.h>

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers
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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,readability-non-const-parameter,cppcoreguidelines-pro-type-union-access,modernize-use-designated-initializers,clang-analyzer-optin.performance.Padding)
// Win32 types; the fakes keep the exact ETW signatures and fill its variable-size layouts (unions,
// GUIDs spelled as the SDK does). Fake is one test's state, not a hot-path struct (padding).

namespace Platform
{
namespace
{

using EtwNet::OwnerLiveness;
using EtwNet::SessionOwner;

constexpr GUID KERNEL_NETWORK = {0x7DD42A49, 0x5329, 0x4832, {0x8D, 0xFD, 0x43, 0xD9, 0x79, 0x15, 0x3A, 0x88}};
constexpr GUID OTHER_PROVIDER = {0x11111111, 0x2222, 0x3333, {0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55}};
constexpr ULONG64 SESSION_ID = 0x5E55;
constexpr ULONG64 CONSUMER_HANDLE = 0xC0;
constexpr ULONG64 INVALID_HANDLE = ~ULONG64{0};

/// One fabricated event: its record, and the payload the record points at.
struct FakeEvent
{
    GUID provider = KERNEL_NETWORK;
    std::uint16_t id = EtwNet::UDP4_SEND_EVENT_ID;
    std::vector<std::byte> payload;
};

FakeEvent udpEvent(std::uint16_t id, std::uint32_t pid, std::uint32_t size, const GUID& provider = KERNEL_NETWORK)
{
    FakeEvent event{.provider = provider, .id = id, .payload = std::vector<std::byte>(EtwNet::UDP6_PAYLOAD_SIZE)};
    event.payload.resize(std::max<std::size_t>(EtwNet::udpPayloadSize(id), 8));
    std::memcpy(event.payload.data(), &pid, sizeof(pid));
    std::memcpy(std::span(event.payload).subspan(sizeof(pid)).data(), &size, sizeof(size));
    return event;
}

/// What the fakes answer and what they saw. One per test (reset in SetUp).
struct Fake
{
    bool elevated = true;
    SessionOwner self{.pid = 100, .creationTime = 555};

    std::vector<ULONG> startStatuses; // Popped per StartTrace call; ERROR_SUCCESS when empty
    std::vector<std::wstring> startedNames;
    EVENT_TRACE_PROPERTIES startProperties{};

    ULONG enableStatusWithFilter = ERROR_SUCCESS;
    ULONG enableStatusWithoutFilter = ERROR_SUCCESS;
    int enableCalls = 0;
    ULONG64 enabledSession = 0;
    GUID enabledProvider{};
    ULONGLONG enabledKeywords = 0;
    UCHAR enabledLevel = 0;
    std::vector<USHORT> filteredIds;
    bool filterIn = false;
    bool lastEnableHadFilter = false;

    ULONG64 openResult = CONSUMER_HANDLE;
    int openCalls = 0;
    std::wstring openedName;
    ULONG openedMode = 0;
    PEVENT_RECORD_CALLBACK callback = nullptr;
    void* context = nullptr;

    std::vector<FakeEvent> events; // Delivered by ProcessTrace before it waits
    ULONG processStatus = ERROR_SUCCESS;

    std::vector<ULONG64> stoppedIds;
    std::vector<std::wstring> stoppedNames;
    int queryCalls = 0;
    ULONG eventsLost = 0;
    int closeCalls = 0;

    std::vector<std::wstring> runningSessions; // What QueryAllTraces lists
    ULONG queryAllStatus = ERROR_SUCCESS;
    std::map<std::uint32_t, OwnerLiveness> liveness; // By PID; Unknown when missing

    // ProcessTrace blocks, as the real one does, until the session stops or the consumer closes.
    std::mutex mutex;
    std::condition_variable changed;
    bool delivered = false;
    bool released = false;
};

Fake& fake()
{
    static Fake instance;
    return instance;
}

void release()
{
    const std::scoped_lock lock(fake().mutex);
    fake().released = true;
    fake().changed.notify_all();
}

ULONG WINAPI fakeStartTrace(ULONG64* traceId, LPCWSTR name, PEVENT_TRACE_PROPERTIES properties)
{
    fake().startedNames.emplace_back(name);
    fake().startProperties = *properties;
    ULONG status = ERROR_SUCCESS;
    if (!fake().startStatuses.empty())
    {
        status = fake().startStatuses.front();
        fake().startStatuses.erase(fake().startStatuses.begin());
    }
    if (status == ERROR_SUCCESS)
    {
        *traceId = SESSION_ID;
        const std::scoped_lock lock(fake().mutex);
        fake().released = false;
        fake().delivered = false;
    }
    return status;
}

ULONG WINAPI fakeControlTrace(ULONG64 traceId, LPCWSTR name, PEVENT_TRACE_PROPERTIES properties, ULONG controlCode)
{
    if (controlCode == EVENT_TRACE_CONTROL_STOP)
    {
        if (name != nullptr)
        {
            fake().stoppedNames.emplace_back(name);
        }
        else
        {
            fake().stoppedIds.push_back(traceId);
            release(); // The session's end makes ProcessTrace return
        }
    }
    else if (controlCode == EVENT_TRACE_CONTROL_QUERY)
    {
        ++fake().queryCalls;
    }
    properties->EventsLost = fake().eventsLost;
    return ERROR_SUCCESS;
}

ULONG WINAPI fakeEnableTraceEx2(ULONG64 traceId,
                                LPCGUID provider,
                                ULONG /*controlCode*/,
                                UCHAR level,
                                ULONGLONG matchAnyKeyword,
                                ULONGLONG /*matchAllKeyword*/,
                                ULONG /*timeout*/,
                                PENABLE_TRACE_PARAMETERS parameters)
{
    ++fake().enableCalls;
    fake().enabledSession = traceId;
    fake().enabledProvider = *provider;
    fake().enabledKeywords = matchAnyKeyword;
    fake().enabledLevel = level;
    fake().lastEnableHadFilter = parameters != nullptr && parameters->FilterDescCount > 0;
    if (!fake().lastEnableHadFilter)
    {
        return fake().enableStatusWithoutFilter;
    }
    const EVENT_FILTER_DESCRIPTOR& descriptor = *parameters->EnableFilterDesc;
    EXPECT_EQ(descriptor.Type, static_cast<ULONG>(EVENT_FILTER_TYPE_EVENT_ID));
    const auto* bytes = reinterpret_cast<const std::byte*>(descriptor.Ptr); // NOLINT(performance-no-int-to-ptr) - the API's own encoding
    EVENT_FILTER_EVENT_ID header{};
    std::memcpy(&header, bytes, offsetof(EVENT_FILTER_EVENT_ID, Events));
    fake().filterIn = header.FilterIn != FALSE;
    fake().filteredIds.assign(header.Count, 0);
    EXPECT_EQ(descriptor.Size, offsetof(EVENT_FILTER_EVENT_ID, Events) + (header.Count * sizeof(USHORT)));
    std::memcpy(fake().filteredIds.data(),
                std::span(bytes, descriptor.Size).subspan(offsetof(EVENT_FILTER_EVENT_ID, Events)).data(),
                header.Count * sizeof(USHORT));
    return fake().enableStatusWithFilter;
}

ULONG64 WINAPI fakeOpenTrace(PEVENT_TRACE_LOGFILEW logfile)
{
    ++fake().openCalls;
    fake().openedName = logfile->LoggerName;
    fake().openedMode = logfile->ProcessTraceMode;
    fake().callback = logfile->EventRecordCallback;
    fake().context = logfile->Context;
    if (fake().openResult == INVALID_HANDLE)
    {
        SetLastError(ERROR_ACCESS_DENIED);
    }
    return fake().openResult;
}

ULONG WINAPI fakeProcessTrace(ULONG64* handles, ULONG handleCount, LPFILETIME /*start*/, LPFILETIME /*end*/)
{
    EXPECT_EQ(handleCount, 1U);
    EXPECT_EQ(*handles, CONSUMER_HANDLE);
    for (FakeEvent& event : fake().events)
    {
        EVENT_RECORD record{};
        record.EventHeader.ProviderId = event.provider;
        record.EventHeader.EventDescriptor.Id = event.id;
        record.UserData = event.payload.empty() ? nullptr : event.payload.data();
        record.UserDataLength = static_cast<USHORT>(event.payload.size());
        record.UserContext = fake().context;
        fake().callback(&record);
    }
    std::unique_lock lock(fake().mutex);
    fake().delivered = true;
    fake().changed.notify_all();
    fake().changed.wait(lock, [] { return fake().released; });
    return fake().processStatus;
}

ULONG WINAPI fakeCloseTrace(ULONG64 handle)
{
    EXPECT_EQ(handle, CONSUMER_HANDLE);
    ++fake().closeCalls;
    release();
    return ERROR_SUCCESS;
}

ULONG WINAPI fakeQueryAllTraces(PEVENT_TRACE_PROPERTIES* properties, ULONG count, PULONG loggerCount)
{
    if (fake().queryAllStatus != ERROR_SUCCESS)
    {
        return fake().queryAllStatus;
    }
    const auto listed = static_cast<ULONG>(std::min<std::size_t>(fake().runningSessions.size(), count));
    ULONG index = 0;
    while (index < listed)
    {
        // The name goes where the caller's LoggerNameOffset says, as QueryAllTraces writes it.
        EVENT_TRACE_PROPERTIES* p = properties[index];
        auto* name = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(p) + p->LoggerNameOffset);
        const std::wstring& source = fake().runningSessions[index];
        std::wmemcpy(name, source.c_str(), source.size() + 1);
        ++index;
    }
    *loggerCount = listed;
    return ERROR_SUCCESS;
}

bool fakeIsElevated()
{
    return fake().elevated;
}

SessionOwner fakeSelf()
{
    return fake().self;
}

OwnerLiveness fakeLiveness(const SessionOwner& owner)
{
    const auto it = fake().liveness.find(owner.pid);
    return (it != fake().liveness.end()) ? it->second : OwnerLiveness::Unknown;
}

WindowsEtwNetCollector::Functions fakeFunctions()
{
    return WindowsEtwNetCollector::Functions{
        .startTraceW = &fakeStartTrace,
        .controlTraceW = &fakeControlTrace,
        .enableTraceEx2 = &fakeEnableTraceEx2,
        .openTraceW = &fakeOpenTrace,
        .processTrace = &fakeProcessTrace,
        .closeTrace = &fakeCloseTrace,
        .queryAllTracesW = &fakeQueryAllTraces,
        .isElevated = &fakeIsElevated,
        .self = &fakeSelf,
        .ownerLiveness = &fakeLiveness,
    };
}

/// Wait (bounded) for the fake ProcessTrace to have delivered its events.
bool waitDelivered()
{
    std::unique_lock lock(fake().mutex);
    return fake().changed.wait_for(lock, std::chrono::seconds(10), [] { return fake().delivered; });
}

class WindowsEtwNetCollectorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Fake& f = fake();
        const std::scoped_lock lock(f.mutex);
        f.elevated = true;
        f.self = SessionOwner{.pid = 100, .creationTime = 555};
        f.startStatuses.clear();
        f.startedNames.clear();
        f.startProperties = {};
        f.enableStatusWithFilter = ERROR_SUCCESS;
        f.enableStatusWithoutFilter = ERROR_SUCCESS;
        f.enableCalls = 0;
        f.enabledSession = 0;
        f.enabledKeywords = 0;
        f.enabledLevel = 0;
        f.filteredIds.clear();
        f.filterIn = false;
        f.lastEnableHadFilter = false;
        f.openResult = CONSUMER_HANDLE;
        f.openCalls = 0;
        f.openedName.clear();
        f.openedMode = 0;
        f.callback = nullptr;
        f.context = nullptr;
        f.events.clear();
        f.processStatus = ERROR_SUCCESS;
        f.stoppedIds.clear();
        f.stoppedNames.clear();
        f.queryCalls = 0;
        f.eventsLost = 0;
        f.closeCalls = 0;
        f.runningSessions.clear();
        f.queryAllStatus = ERROR_SUCCESS;
        f.liveness.clear();
        f.delivered = false;
        f.released = false;
    }
};

/// @p pid's totals, or an empty entry (PID 0) when it has none. A plain value keeps clang-tidy's
/// optional dataflow satisfied: bugprone-unchecked-optional-access doesn't model ASSERT_TRUE.
UdpProcessTraffic trafficOf(const std::vector<UdpProcessTraffic>& totals, std::uint32_t pid)
{
    const auto it = std::ranges::find(totals, pid, &UdpProcessTraffic::pid);
    return (it != totals.end()) ? *it : UdpProcessTraffic{};
}

// --- Start failures ---------------------------------------------------------------------------

TEST_F(WindowsEtwNetCollectorTest, NotElevatedNothingIsTried)
{
    fake().elevated = false;
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::NotElevated);
    EXPECT_TRUE(fake().startedNames.empty());
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_TRUE(collector.sample().empty());
}

TEST_F(WindowsEtwNetCollectorTest, ADeniedStartTraceLeavesItStopped)
{
    fake().startStatuses = {ERROR_ACCESS_DENIED};
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Denied);
    EXPECT_EQ(fake().enableCalls, 0);
    EXPECT_EQ(fake().openCalls, 0);
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_TRUE(collector.sessionName().empty());
    EXPECT_TRUE(fake().stoppedIds.empty()); // Nothing was started, so nothing is stopped
}

TEST_F(WindowsEtwNetCollectorTest, AnyOtherStartFailureIsFailed)
{
    fake().startStatuses = {ERROR_NO_SYSTEM_RESOURCES};
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Failed);
    EXPECT_FALSE(collector.isDelivering());
}

TEST_F(WindowsEtwNetCollectorTest, ARefusedProviderStopsTheSessionItStarted)
{
    fake().enableStatusWithFilter = ERROR_ACCESS_DENIED;
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Denied);
    EXPECT_EQ(fake().stoppedIds, std::vector<ULONG64>{SESSION_ID});
    EXPECT_EQ(fake().openCalls, 0);
    EXPECT_FALSE(collector.isDelivering());
}

TEST_F(WindowsEtwNetCollectorTest, AFailedOpenTraceStopsTheSessionItStarted)
{
    fake().openResult = INVALID_HANDLE;
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Failed);
    EXPECT_EQ(fake().stoppedIds, std::vector<ULONG64>{SESSION_ID});
    EXPECT_EQ(fake().closeCalls, 0); // No consumer handle to close
    EXPECT_FALSE(collector.isDelivering());
}

// --- A running session ------------------------------------------------------------------------

TEST_F(WindowsEtwNetCollectorTest, StartsANamedRealTimeSessionWithOnlyTheUdpEvents)
{
    WindowsEtwNetCollector collector(fakeFunctions());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_TRUE(waitDelivered());

    EXPECT_EQ(fake().startedNames, std::vector<std::wstring>{L"TaskSmack-UDP-100-555"});
    EXPECT_EQ(collector.sessionName(), L"TaskSmack-UDP-100-555");
    const EVENT_TRACE_PROPERTIES& props = fake().startProperties;
    EXPECT_EQ(props.LogFileMode, static_cast<ULONG>(EVENT_TRACE_REAL_TIME_MODE));
    EXPECT_EQ(props.BufferSize, WindowsEtwNetCollector::BUFFER_SIZE_KB);
    EXPECT_EQ(props.MinimumBuffers, WindowsEtwNetCollector::MINIMUM_BUFFERS);
    EXPECT_EQ(props.MaximumBuffers, WindowsEtwNetCollector::MAXIMUM_BUFFERS);
    EXPECT_EQ(props.FlushTimer, WindowsEtwNetCollector::FLUSH_TIMER_SECONDS);
    EXPECT_NE(props.LoggerNameOffset, 0U);

    EXPECT_EQ(fake().enabledSession, SESSION_ID);
    EXPECT_TRUE(IsEqualGUID(fake().enabledProvider, KERNEL_NETWORK));
    EXPECT_EQ(fake().enabledKeywords, EtwNet::KEYWORD_IPV4 | EtwNet::KEYWORD_IPV6);
    EXPECT_EQ(fake().enabledLevel, TRACE_LEVEL_INFORMATION);
    EXPECT_TRUE(fake().lastEnableHadFilter);
    EXPECT_TRUE(fake().filterIn);
    EXPECT_EQ(fake().filteredIds, (std::vector<USHORT>{42, 43, 58, 59}));

    EXPECT_EQ(fake().openedName, L"TaskSmack-UDP-100-555");
    EXPECT_EQ(fake().openedMode, static_cast<ULONG>(PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD));
    EXPECT_EQ(fake().context, &collector);

    // Started, but no UDP event yet: the counters don't include UDP until one arrives.
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started); // Already running: not restarted
    EXPECT_EQ(fake().startedNames.size(), 1U);

    collector.stop();
    EXPECT_EQ(fake().stoppedIds, std::vector<ULONG64>{SESSION_ID});
    EXPECT_EQ(fake().closeCalls, 1);
    EXPECT_TRUE(collector.sessionName().empty());
    collector.stop(); // Stopped already: nothing more
    EXPECT_EQ(fake().stoppedIds.size(), 1U);
    EXPECT_EQ(fake().closeCalls, 1);
}

TEST_F(WindowsEtwNetCollectorTest, UdpEventsAreSummedPerProcessAndEverythingElseIgnored)
{
    std::vector<FakeEvent> events{
        udpEvent(EtwNet::UDP4_SEND_EVENT_ID, 10, 100),
        udpEvent(EtwNet::UDP4_RECV_EVENT_ID, 10, 50),
        udpEvent(EtwNet::UDP6_SEND_EVENT_ID, 20, 7),
        udpEvent(EtwNet::UDP6_RECV_EVENT_ID, 20, 3),
        udpEvent(10, 10, 1'000),                                         // TCPv4 send: EStats counts it
        udpEvent(EtwNet::UDP4_SEND_EVENT_ID, 10, 1'000, OTHER_PROVIDER), // Another provider's event 42
        udpEvent(EtwNet::UDP4_SEND_EVENT_ID, 0, 1'000),                  // PID 0
    };
    FakeEvent truncated = udpEvent(EtwNet::UDP6_SEND_EVENT_ID, 20, 1'000);
    truncated.payload.resize(EtwNet::UDP4_PAYLOAD_SIZE);
    events.push_back(truncated);
    fake().events = events;

    WindowsEtwNetCollector collector(fakeFunctions());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_TRUE(waitDelivered());
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_EQ(collector.ignoredEvents(), 4U);

    const std::vector<UdpProcessTraffic> totals = collector.sample();
    ASSERT_EQ(totals.size(), 2U);
    // trafficOf() gives an empty entry for a missing process, which fails the checks below.
    const UdpProcessTraffic p10 = trafficOf(totals, 10);
    const UdpProcessTraffic p20 = trafficOf(totals, 20);
    EXPECT_EQ(p10.pid, 10U);
    EXPECT_EQ(p10.bytesSent, 100U);
    EXPECT_EQ(p10.bytesReceived, 50U);
    EXPECT_EQ(p20.pid, 20U);
    EXPECT_EQ(p20.bytesSent, 7U);
    EXPECT_EQ(p20.bytesReceived, 3U);
    EXPECT_EQ(fake().queryCalls, 1); // The first sample logs the lost events

    collector.stop();
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_TRUE(collector.sample().empty()); // Stopped: the counters are forgotten
}

TEST_F(WindowsEtwNetCollectorTest, WithoutEventIdFilteringTheCallbackFiltersAlone)
{
    fake().enableStatusWithFilter = ERROR_NOT_SUPPORTED;
    fake().events = {udpEvent(11, 10, 1'000), udpEvent(EtwNet::UDP4_RECV_EVENT_ID, 10, 64)};
    WindowsEtwNetCollector collector(fakeFunctions());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_EQ(fake().enableCalls, 2);
    EXPECT_FALSE(fake().lastEnableHadFilter);
    ASSERT_TRUE(waitDelivered());
    const std::vector<UdpProcessTraffic> totals = collector.sample();
    ASSERT_EQ(totals.size(), 1U);
    EXPECT_EQ(totals[0].bytesReceived, 64U); // The TCP receive is not counted
    EXPECT_EQ(totals[0].bytesSent, 0U);
}

TEST_F(WindowsEtwNetCollectorTest, ASessionEndedFromOutsideStopsDeliveringAndCanStartAgain)
{
    fake().events = {udpEvent(EtwNet::UDP4_SEND_EVENT_ID, 10, 1)};
    fake().processStatus = ERROR_CANCELLED;
    WindowsEtwNetCollector collector(fakeFunctions());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_TRUE(waitDelivered());
    EXPECT_TRUE(collector.isDelivering());

    release(); // logman stop: ProcessTrace returns while the collector still thinks it runs
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (collector.isDelivering() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_TRUE(collector.sample().empty());

    // Starting again cleans up the ended one (stop, close, join) and starts a new session.
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_EQ(fake().closeCalls, 1);
    EXPECT_EQ(fake().startedNames.size(), 2U);
    ASSERT_TRUE(waitDelivered());
    collector.stop();
    EXPECT_EQ(fake().closeCalls, 2);
}

TEST_F(WindowsEtwNetCollectorTest, TheDestructorStopsTheSession)
{
    {
        WindowsEtwNetCollector collector(fakeFunctions());
        ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
        ASSERT_TRUE(waitDelivered());
    }
    EXPECT_EQ(fake().stoppedIds, std::vector<ULONG64>{SESSION_ID});
    EXPECT_EQ(fake().closeCalls, 1);
}

TEST_F(WindowsEtwNetCollectorTest, ACallbackWithoutAContextDoesNothing)
{
    EVENT_RECORD record{};
    WindowsEtwNetCollector::onEventRecord(&record);
    WindowsEtwNetCollector::onEventRecord(nullptr);

    WindowsEtwNetCollector collector(fakeFunctions());
    const FakeEvent event = udpEvent(EtwNet::UDP4_SEND_EVENT_ID, 10, 5);
    record.EventHeader.ProviderId = KERNEL_NETWORK;
    record.EventHeader.EventDescriptor.Id = event.id;
    record.UserData = nullptr; // A record that claims a payload but has none
    record.UserDataLength = static_cast<USHORT>(event.payload.size());
    collector.handleEvent(record);
    EXPECT_EQ(collector.ignoredEvents(), 1U);
}

// --- Stale sessions ---------------------------------------------------------------------------

TEST_F(WindowsEtwNetCollectorTest, OnlyACrashedTaskSmacksSessionIsStopped)
{
    fake().runningSessions = {
        L"NT Kernel Logger",
        L"TaskSmack-UDP-200-1",   // Owner gone: stopped
        L"TaskSmack-UDP-300-2",   // Another running TaskSmack: left alone
        L"TaskSmack-UDP-400-3",   // PID reused by another process: stopped
        L"TaskSmack-UDP-500-4",   // Couldn't be checked: left alone
        L"TaskSmack-UDP-100-555", // This process's own leftover: stopped
        L"TaskSmack-UDP-junk",
        L"Diagtrack-Listener",
    };
    fake().liveness = {{200, OwnerLiveness::Gone}, {300, OwnerLiveness::Alive}, {400, OwnerLiveness::PidReused}};
    WindowsEtwNetCollector collector(fakeFunctions());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_EQ(fake().stoppedNames, (std::vector<std::wstring>{L"TaskSmack-UDP-200-1", L"TaskSmack-UDP-400-3", L"TaskSmack-UDP-100-555"}));
    ASSERT_TRUE(waitDelivered());
}

TEST_F(WindowsEtwNetCollectorTest, AFailedSessionListingStillStarts)
{
    fake().queryAllStatus = ERROR_ACCESS_DENIED;
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_TRUE(fake().stoppedNames.empty());
    ASSERT_TRUE(waitDelivered());
}

TEST_F(WindowsEtwNetCollectorTest, ASessionOfTheSameNameIsReplaced)
{
    fake().startStatuses = {ERROR_ALREADY_EXISTS};
    WindowsEtwNetCollector collector(fakeFunctions());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_EQ(fake().stoppedNames, std::vector<std::wstring>{L"TaskSmack-UDP-100-555"});
    EXPECT_EQ(fake().startedNames.size(), 2U);
    ASSERT_TRUE(waitDelivered());
}

// --- The real calls ---------------------------------------------------------------------------

TEST(WindowsEtwNetCollectorSystemTest, TheRealFunctionsAreAllSet)
{
    const WindowsEtwNetCollector::Functions fns = WindowsEtwNetCollector::systemFunctions();
    EXPECT_NE(fns.startTraceW, nullptr);
    EXPECT_NE(fns.controlTraceW, nullptr);
    EXPECT_NE(fns.enableTraceEx2, nullptr);
    EXPECT_NE(fns.openTraceW, nullptr);
    EXPECT_NE(fns.processTrace, nullptr);
    EXPECT_NE(fns.closeTrace, nullptr);
    EXPECT_NE(fns.queryAllTracesW, nullptr);
    ASSERT_NE(fns.self, nullptr);
    ASSERT_NE(fns.ownerLiveness, nullptr);
    // This process, checked against itself: alive with its own creation time; and with another
    // creation time, a reused PID. Neither starts a session.
    const SessionOwner self = fns.self();
    EXPECT_EQ(self.pid, GetCurrentProcessId());
    EXPECT_NE(self.creationTime, 0U);
    EXPECT_EQ(fns.ownerLiveness(self), OwnerLiveness::Alive);
    EXPECT_EQ(fns.ownerLiveness(SessionOwner{.pid = self.pid, .creationTime = self.creationTime + 1}), OwnerLiveness::PidReused);
}

} // namespace
} // namespace Platform

// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,readability-non-const-parameter,cppcoreguidelines-pro-type-union-access,modernize-use-designated-initializers,clang-analyzer-optin.performance.Padding)
