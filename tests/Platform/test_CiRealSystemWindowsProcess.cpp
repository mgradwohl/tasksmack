// CI real-system tests on Windows, phase 2 (#3490; tracker #3492): process paths that until now had only fakes
// and a manual check. A window that stops answering reads as Not responding, and its wait chain leads to the
// thread it waits on (#3084, #3211); the UDP trace counts a process's datagrams from a real ETW session and leaves
// none behind (#1492); SeDebugPrivilege is enabled on a real token (#2128); the critical-process guard refuses
// winlogon.exe with the real IsProcessCritical, its one changing call a fake (#2556); and the tests that skip
// because the runner is elevated run again as a standard user. They need an elevated runner and change what runs on
// it, so they run only where CiRealSystem.h's guard lets them; each ends what it starts and puts back what it changes.

#include "Platform/IProcessActions.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"
#include "Platform/WaitChain.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsDebugPrivilege.h"
#include "Platform/Windows/WindowsEtwNetCollector.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Support/CiRealSystem.h"
#include "Support/CiRealSystemWindows.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <evntrace.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#ifndef TASKSMACK_CI_STUB_PATH
#error "TASKSMACK_CI_STUB_PATH must name TaskSmackCiStub.exe (tests/CMakeLists.txt)"
#endif

namespace
{

using namespace std::chrono_literals;
using namespace TestSupport::CiRealSystem;
using Platform::ProcessActionResult;
using Platform::Windows::UniqueHandle;

// --- #3084 / #3211: Not responding, and the wait chain ---------------------------------------------------------

class CiRealSystemHungWindowTest : public TestSupport::CiRealSystemTest
{};

/// The visible top-level window of process @p pid, within @p timeout; null when it has none.
[[nodiscard]] HWND visibleWindowOf(DWORD pid, std::chrono::milliseconds timeout)
{
    struct Search
    {
        DWORD pid = 0;
        HWND found = nullptr;
    };
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        Search search{.pid = pid};
        EnumWindows(
            [](HWND window, LPARAM param) -> BOOL
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr) - EnumWindows' context
                auto* wanted = reinterpret_cast<Search*>(param);
                DWORD owner = 0;
                GetWindowThreadProcessId(window, &owner);
                if (owner == wanted->pid && IsWindowVisible(window) != FALSE)
                {
                    wanted->found = window;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        if (search.found != nullptr)
        {
            return search.found;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return nullptr;
}

/// Whether the process probe marks @p pid Not responding, within @p timeout.
[[nodiscard]] bool probeSaysNotResponding(DWORD pid, std::chrono::milliseconds timeout)
{
    Platform::WindowsProcessProbe probe;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        const std::vector<Platform::ProcessCounters> processes = probe.enumerate();
        const auto found = std::ranges::find(processes, static_cast<std::int32_t>(pid), &Platform::ProcessCounters::pid);
        if (found != processes.end() && found->notResponding)
        {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

TEST_F(CiRealSystemHungWindowTest, AWindowThatStopsAnsweringIsNotRespondingAndItsChainLeadsToTheHolder)
{
    // TaskSmackCiStub.exe --hung-window: a window it answers for a moment, then its main thread blocks on a mutex
    // another of its threads holds. Windows calls a window hung once its thread hasn't asked for messages for
    // about five seconds.
    const Child stub = start(TASKSMACK_CI_STUB_PATH, {.parameters = L"--hung-window 90000"});
    ASSERT_NE(stub.process(), nullptr);
    HWND window = visibleWindowOf(stub.pid(), STARTUP_WAIT);
    if (window == nullptr && GetShellWindow() == nullptr)
    {
        GTEST_SKIP() << "the stub's window never showed, and this runner's desktop has no shell";
    }
    ASSERT_NE(window, nullptr) << "the stub's window never showed";

    ASSERT_TRUE(probeSaysNotResponding(stub.pid(), 30s))
        << "the probe never marked the stub Not responding; IsHungAppWindow says " << IsHungAppWindow(window);

    Platform::WindowsProcessActions actions;
    const Platform::WaitChainReadResult chains = actions.readWaitChains(stub.target(), std::stop_token{});
    ASSERT_EQ(chains.status, Platform::WaitChainReadStatus::Ok) << chains.detail;
    // The main thread's chain: the thread, the mutex it waits on, then the stub's other thread, which owns it.
    bool ledToTheHolder = false;
    for (const Platform::ThreadWaitChain& chain : chains.threads)
    {
        for (std::size_t at = 1; at + 1 < chain.nodes.size(); ++at)
        {
            const Platform::WaitChainNode& object = chain.nodes[at];
            const Platform::WaitChainNode& owner = chain.nodes[at + 1];
            if (object.type == Platform::WaitObjectType::Mutex && owner.type == Platform::WaitObjectType::Thread &&
                std::cmp_equal(owner.pid, stub.pid()) && owner.tid != chain.tid)
            {
                ledToTheHolder = true;
            }
        }
    }
    EXPECT_TRUE(ledToTheHolder) << "no chain of the stub's " << chains.threads.size() << " threads led through a mutex to its other thread";
}

// --- #1492: the UDP trace ----------------------------------------------------------------------------------

class CiRealSystemUdpTraceTest : public TestSupport::CiRealSystemTest
{};

/// Winsock, started for the test and cleaned up on scope exit.
class Winsock
{
  public:
    Winsock()
    {
        WSADATA data{};
        m_Started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock()
    {
        if (m_Started)
        {
            WSACleanup();
        }
    }
    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
    Winsock(Winsock&&) = delete;
    Winsock& operator=(Winsock&&) = delete;

    [[nodiscard]] bool started() const noexcept
    {
        return m_Started;
    }

  private:
    bool m_Started = false;
};

/// A UDP socket, closed on scope exit.
class UdpSocket
{
  public:
    UdpSocket() : m_Socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))
    {}
    ~UdpSocket()
    {
        if (m_Socket != INVALID_SOCKET) // NOLINT(modernize-use-integer-sign-comparison) - INVALID_SOCKET is ~0 as a SOCKET
        {
            closesocket(m_Socket);
        }
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) = delete;
    UdpSocket& operator=(UdpSocket&&) = delete;

    [[nodiscard]] SOCKET get() const noexcept
    {
        return m_Socket;
    }

  private:
    SOCKET m_Socket;
};

/// How many datagrams each burst sends over loopback, and how big each one is.
constexpr int DATAGRAMS = 50;
constexpr int DATAGRAM_BYTES = 1000;

/// Sends DATAGRAMS datagrams from @p sender to @p receiver's @p address and reads them back: how many arrived.
int sendBurst(const UdpSocket& sender, const UdpSocket& receiver, const sockaddr_in& address)
{
    std::array<char, DATAGRAM_BYTES> payload{};
    payload.fill('u');
    for (int i = 0; i < DATAGRAMS; ++i)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes a sockaddr
        sendto(sender.get(), payload.data(), DATAGRAM_BYTES, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    }
    int arrived = 0;
    std::array<char, DATAGRAM_BYTES> buffer{};
    while (arrived < DATAGRAMS && recv(receiver.get(), buffer.data(), DATAGRAM_BYTES, 0) == DATAGRAM_BYTES)
    {
        ++arrived;
    }
    return arrived;
}

/// This process's UDP counters in @p traffic; zeros when it isn't listed.
[[nodiscard]] Platform::UdpProcessTraffic ownTraffic(const std::vector<Platform::UdpProcessTraffic>& traffic)
{
    const auto found = std::ranges::find(traffic, static_cast<std::uint32_t>(GetCurrentProcessId()), &Platform::UdpProcessTraffic::pid);
    return (found != traffic.end()) ? *found : Platform::UdpProcessTraffic{.pid = GetCurrentProcessId()};
}

/// Whether an ETW session named @p name runs (ControlTraceW's query finds it).
[[nodiscard]] bool traceSessionRuns(const std::wstring& name)
{
    struct Properties
    {
        EVENT_TRACE_PROPERTIES properties;
        std::array<wchar_t, 1024> loggerName;
    };
    Properties query{};
    query.properties.Wnode.BufferSize = sizeof(query);
    query.properties.LoggerNameOffset = offsetof(Properties, loggerName);
    return ControlTraceW(0, name.c_str(), &query.properties, EVENT_TRACE_CONTROL_QUERY) != ERROR_WMI_INSTANCE_NOT_FOUND;
}

TEST_F(CiRealSystemUdpTraceTest, TheSessionCountsThisProcesssLoopbackUdpAndIsGoneAfterStop)
{
    const Winsock winsock;
    ASSERT_TRUE(winsock.started()) << WSAGetLastError();
    const UdpSocket receiver;
    const UdpSocket sender;
    ASSERT_NE(receiver.get(), INVALID_SOCKET) << WSAGetLastError();
    ASSERT_NE(sender.get(), INVALID_SOCKET) << WSAGetLastError();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes a sockaddr
    ASSERT_EQ(bind(receiver.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0) << WSAGetLastError();
    int length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes a sockaddr
    ASSERT_EQ(getsockname(receiver.get(), reinterpret_cast<sockaddr*>(&address), &length), 0) << WSAGetLastError();
    const DWORD receiveTimeoutMs = 2000;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - setsockopt takes the option as bytes
    setsockopt(receiver.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs));

    std::wstring session;
    {
        Platform::WindowsEtwNetCollector collector;
        ASSERT_EQ(collector.start(), Platform::UdpCollectorStartResult::Started);
        session = collector.sessionName();
        ASSERT_FALSE(session.empty());
        EXPECT_TRUE(traceSessionRuns(session));

        // Send until the session delivers its first event: the provider is enabled a moment after start().
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!collector.isDelivering() && std::chrono::steady_clock::now() < deadline)
        {
            [[maybe_unused]] const int warmUp = sendBurst(sender, receiver, address);
            std::this_thread::sleep_for(200ms);
        }
        ASSERT_TRUE(collector.isDelivering()) << "the session never delivered a UDP event";

        // Then one counted burst: this process's counters grow by at least its bytes, both ways.
        const Platform::UdpProcessTraffic before = ownTraffic(collector.sample());
        ASSERT_EQ(sendBurst(sender, receiver, address), DATAGRAMS);
        constexpr std::uint64_t BURST_BYTES = static_cast<std::uint64_t>(DATAGRAMS) * DATAGRAM_BYTES;
        Platform::UdpProcessTraffic after = before;
        const auto counted = std::chrono::steady_clock::now() + 15s;
        while ((after.bytesSent < before.bytesSent + BURST_BYTES || after.bytesReceived < before.bytesReceived + BURST_BYTES) &&
               std::chrono::steady_clock::now() < counted)
        {
            std::this_thread::sleep_for(250ms);
            after = ownTraffic(collector.sample());
        }
        EXPECT_GE(after.bytesSent - before.bytesSent, BURST_BYTES);
        EXPECT_GE(after.bytesReceived - before.bytesReceived, BURST_BYTES);
        RecordProperty("udpBytesSent", std::to_string(after.bytesSent));

        collector.stop();
        EXPECT_FALSE(collector.isDelivering());
        EXPECT_TRUE(collector.sample().empty());
    }
    EXPECT_FALSE(traceSessionRuns(session)) << "the UDP trace session outlived its collector";
}

// --- #2128: SeDebugPrivilege, and #2556: the critical-process guard ------------------------------------------

/// Whether SeDebugPrivilege is enabled on this process's token.
[[nodiscard]] bool debugPrivilegeEnabled()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    {
        return false;
    }
    const UniqueHandle owned(token);
    LUID debug{};
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debug) == FALSE)
    {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(owned.get(), TokenPrivileges, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (size == 0 || GetTokenInformation(owned.get(), TokenPrivileges, buffer.data(), size, &size) == FALSE)
    {
        return false;
    }
    const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(
        buffer.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - the token's TOKEN_PRIVILEGES
    const std::span<const LUID_AND_ATTRIBUTES> held(&privileges->Privileges[0], privileges->PrivilegeCount);
    return std::ranges::any_of(held,
                               [&debug](const LUID_AND_ATTRIBUTES& privilege)
                               {
                                   return privilege.Luid.LowPart == debug.LowPart && privilege.Luid.HighPart == debug.HighPart &&
                                          (privilege.Attributes & SE_PRIVILEGE_ENABLED) != 0;
                               });
}

/// Turns SeDebugPrivilege off again on scope exit, unless it was on before: the later tests in this process run
/// with the token they started with.
class DebugPrivilegeRestorer
{
  public:
    DebugPrivilegeRestorer() : m_WasEnabled(debugPrivilegeEnabled())
    {}
    ~DebugPrivilegeRestorer()
    {
        if (m_WasEnabled)
        {
            return;
        }
        HANDLE token = nullptr;
        LUID debug{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token) == FALSE)
        {
            return;
        }
        const UniqueHandle owned(token);
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debug) != FALSE)
        {
            TOKEN_PRIVILEGES off{.PrivilegeCount = 1, .Privileges = {{.Luid = debug, .Attributes = 0}}};
            AdjustTokenPrivileges(owned.get(), FALSE, &off, sizeof(off), nullptr, nullptr);
        }
    }
    DebugPrivilegeRestorer(const DebugPrivilegeRestorer&) = delete;
    DebugPrivilegeRestorer& operator=(const DebugPrivilegeRestorer&) = delete;
    DebugPrivilegeRestorer(DebugPrivilegeRestorer&&) = delete;
    DebugPrivilegeRestorer& operator=(DebugPrivilegeRestorer&&) = delete;

  private:
    bool m_WasEnabled;
};

class CiRealSystemDebugPrivilegeTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemDebugPrivilegeTest, TheElevatedTokenGetsSeDebugEnabled)
{
    // main.cpp's start-up call (#2128), on this elevated process's real token, checked on the token itself.
    const DebugPrivilegeRestorer restorer;
    EXPECT_EQ(Platform::Windows::enableDebugPrivilegeIfElevated(), Platform::Windows::DebugPrivilegeResult::Enabled);
    EXPECT_TRUE(debugPrivilegeEnabled());
}

class CiRealSystemCriticalProcessTest : public TestSupport::CiRealSystemTest
{};

/// How many times the fakes below were called: a guard that let the change through.
[[nodiscard]] std::atomic<int>& priorityCalls()
{
    static std::atomic<int> calls{0};
    return calls;
}

TEST_F(CiRealSystemCriticalProcessTest, WinlogonIsCriticalAndAPriorityChangeIsRefused)
{
    // Read-only: the real IsProcessCritical answers on csrss.exe and winlogon.exe, and the guard refuses a priority
    // change on winlogon.exe before its call, which is a fake here, so even a broken guard changes nothing.
    const DebugPrivilegeRestorer restorer;
    ASSERT_EQ(Platform::Windows::enableDebugPrivilegeIfElevated(), Platform::Windows::DebugPrivilegeResult::Enabled);
    for (const std::wstring_view name : {std::wstring_view(L"csrss.exe"), std::wstring_view(L"winlogon.exe")})
    {
        const std::vector<DWORD> pids = pidsNamed(name);
        ASSERT_FALSE(pids.empty()) << "no " << Platform::WinString::wideToUtf8(name) << " runs";
        const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids.front()));
        ASSERT_TRUE(process) << GetLastError();
        BOOL critical = FALSE;
        ASSERT_NE(IsProcessCritical(process.get(), &critical), FALSE) << GetLastError();
        EXPECT_NE(critical, FALSE) << Platform::WinString::wideToUtf8(name) << " isn't critical";
    }

    const std::vector<DWORD> winlogons = pidsNamed(L"winlogon.exe");
    ASSERT_FALSE(winlogons.empty());
    const UniqueHandle winlogon(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, winlogons.front()));
    ASSERT_TRUE(winlogon) << GetLastError();
    const Platform::ProcessTarget target{
        .pid = static_cast<std::int32_t>(winlogons.front()),
        .startTimeTicks = startTicksOf(winlogon.get()),
    };

    Platform::WindowsProcessFunctions functions = Platform::realWindowsProcessFunctions();
    priorityCalls() = 0;
    functions.setPriorityClass = [](HANDLE /*process*/, DWORD /*priorityClass*/) -> BOOL
    {
        ++priorityCalls();
        return TRUE;
    };
    functions.getPriorityClass = [](HANDLE /*process*/) -> DWORD
    {
        ++priorityCalls();
        return NORMAL_PRIORITY_CLASS;
    };
    Platform::WindowsProcessActions actions(functions);
    const ProcessActionResult result = actions.setPriority(target, 0);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied) << "a critical process is refused, not retried as administrator";
    EXPECT_EQ(result.errorMessage, Platform::criticalProcessRefusal(target.pid, Platform::GuardedAction::Priority));
    EXPECT_EQ(priorityCalls().load(), 0) << "the guard let a priority change on winlogon.exe through";
}

// --- The tests that skip only because the runner is elevated, run as a standard user -------------------------

class CiRealSystemStandardUserTest : public TestSupport::CiRealSystemTest
{};

/// The tests that skip on an elevated runner, and pass as a standard user.
constexpr std::wstring_view ELEVATED_ONLY_SKIPS =
    L"WindowsProcessProbeTest.NonElevatedNeverClaimsNetworkCounters:TaskManagerInstallTest.TheElevatedRouteAndTheHelperReachTheSameVerdict";

/// How many times @p needle is in @p text.
[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size()))
    {
        ++count;
    }
    return count;
}

TEST_F(CiRealSystemStandardUserTest, TheElevatedOnlySkipsRunAsAStandardUser)
{
    // This binary again, with a standard user's primary token (the runner's own account, Administrators deny-only,
    // Medium integrity), running only those tests; its JSON report says each ran and passed.
    const UniqueHandle token = standardUserPrimaryToken();
    ASSERT_TRUE(token);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    ASSERT_NE(GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size), FALSE) << GetLastError();
    ASSERT_EQ(elevation.TokenIsElevated, 0U) << "the standard user's token still reads as elevated";

    const ScratchFolder folder(std::filesystem::temp_directory_path() / (L"TaskSmackCiStandardUser." + uniqueSuffix()));
    const std::filesystem::path report = folder.path() / L"report.json";
    const std::wstring parameters =
        L"--gtest_filter=" + std::wstring(ELEVATED_ONLY_SKIPS) + L" \"--gtest_output=json:" + report.wstring() + L"\"";
    const Child child = start(ownExecutable(), {.token = token.get(), .parameters = parameters});
    ASSERT_NE(child.process(), nullptr);
    const std::optional<DWORD> exit = child.exitCode(2 * STARTUP_WAIT);
    ASSERT_TRUE(exit.has_value()) << "the standard-user run didn't finish";
    std::ifstream in(report, std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(json.empty()) << "the standard-user run wrote no report; its exit status " << exit.value_or(0);
    EXPECT_EQ(exit.value_or(1), 0U) << json;
    EXPECT_EQ(occurrences(json, "\"result\": \"COMPLETED\""), 2U) << json;
    EXPECT_EQ(occurrences(json, "\"result\": \"SKIPPED\""), 0U) << json;
    EXPECT_EQ(occurrences(json, "\"failure\""), 0U) << json;
}

} // namespace
