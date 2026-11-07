// CI real-system tests on Windows, phase 2 (#3490; tracker #3492): process paths that until now had only fakes
// and a manual check. A window that stops answering reads as Not responding, and its wait chain leads to the
// thread it waits on (#3084, #3211); the UDP trace counts a process's datagrams from a real ETW session and leaves
// none behind (#1492); SeDebugPrivilege is enabled on a real token (#2128); the critical-process guard refuses
// a critical process with the real IsProcessCritical, its one changing call a fake (#2556); a 32-bit (WOW64)
// process's environment is read through its PEB32 (#3364); and the tests that skip because the runner is elevated
// run again as a throwaway standard account. They need an elevated runner and change what runs on it, so they run
// only where CiRealSystem.h's guard lets them; each ends what it starts and puts back what it changes.

#include "Core/Utf8Path.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"
#include "Platform/WaitChain.h"
#include "Platform/Windows/WindowsDebugPrivilege.h"
#include "Platform/Windows/WindowsEtwNetCollector.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsProcessEnvironmentReader.h"
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
#include <aclapi.h>
#include <evntrace.h>
#include <tlhelp32.h>
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
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
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

/// Sends DATAGRAMS datagrams from @p sender to @p receiver's @p address, reading each back before the next is sent,
/// so the receive buffer never holds more than one and none is dropped: how many were sent whole and arrived whole.
/// A send or a receive that fails, or comes up short, ends the burst there.
int sendBurst(const UdpSocket& sender, const UdpSocket& receiver, const sockaddr_in& address)
{
    std::array<char, DATAGRAM_BYTES> payload{};
    payload.fill('u');
    std::array<char, DATAGRAM_BYTES> buffer{};
    int arrived = 0;
    for (int i = 0; i < DATAGRAMS; ++i)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Winsock takes a sockaddr
        const int sent =
            sendto(sender.get(), payload.data(), DATAGRAM_BYTES, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent != DATAGRAM_BYTES || recv(receiver.get(), buffer.data(), DATAGRAM_BYTES, 0) != DATAGRAM_BYTES)
        {
            break;
        }
        ++arrived;
    }
    return arrived;
}

/// This process's UDP counters in @p traffic; nullopt when it isn't listed (an empty sample: a consumer that stopped).
[[nodiscard]] std::optional<Platform::UdpProcessTraffic> ownTraffic(const std::vector<Platform::UdpProcessTraffic>& traffic)
{
    const auto found = std::ranges::find(traffic, static_cast<std::uint32_t>(GetCurrentProcessId()), &Platform::UdpProcessTraffic::pid);
    if (found == traffic.end())
    {
        return std::nullopt;
    }
    return *found;
}

/// How long this process's counters must stay the same before the baseline is taken: several of the session's
/// flushes (FLUSH_TIMER_SECONDS), so no warm-up event is still on its way.
constexpr std::chrono::milliseconds QUIET_FOR{3'000};

/// This process's counters once they have stayed the same for QUIET_FOR, within @p timeout, with nothing sent
/// meanwhile: every warm-up event that will ever arrive has. nullopt when they never settle, or the process
/// drops out of the sample.
[[nodiscard]] std::optional<Platform::UdpProcessTraffic> settledTraffic(Platform::WindowsEtwNetCollector& collector,
                                                                        std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<Platform::UdpProcessTraffic> last = ownTraffic(collector.sample());
    auto stableSince = std::chrono::steady_clock::now();
    while (last.has_value() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(250ms);
        const std::optional<Platform::UdpProcessTraffic> now = ownTraffic(collector.sample());
        if (!now.has_value())
        {
            return std::nullopt;
        }
        if (now->bytesSent != last->bytesSent || now->bytesReceived != last->bytesReceived)
        {
            last = now;
            stableSince = std::chrono::steady_clock::now();
        }
        else if (std::chrono::steady_clock::now() - stableSince >= QUIET_FOR)
        {
            return last;
        }
    }
    return std::nullopt;
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

        // Send until the session counts this process: the provider is enabled a moment after start(), and
        // isDelivering() turns true on any process's first event, not necessarily this one's.
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        bool counted = false;
        while (!counted && std::chrono::steady_clock::now() < deadline)
        {
            [[maybe_unused]] const int warmUp = sendBurst(sender, receiver, address);
            std::this_thread::sleep_for(200ms);
            counted = ownTraffic(collector.sample()).has_value();
        }
        ASSERT_TRUE(collector.isDelivering()) << "the session never delivered a UDP event";
        ASSERT_TRUE(counted) << "the session never counted this process's UDP";

        // The warm-up's events may still be on their way, and would count toward the burst: so the baseline is
        // taken only once this process's counters have stopped changing, with nothing sent. (Waiting for the whole
        // warm-up volume can't work: what was sent before the provider was enabled is never seen.)
        const std::optional<Platform::UdpProcessTraffic> settled = settledTraffic(collector, 30s);
        ASSERT_TRUE(settled.has_value()) << "this process's UDP counters never settled, or it dropped out of the sample";
        const Platform::UdpProcessTraffic before = settled.value_or(Platform::UdpProcessTraffic{});

        // Then one counted burst: this process's cumulative counters reach at least the baseline plus its bytes, both
        // ways. A sample without this process (a consumer that stopped) fails, never reads as zeros.
        ASSERT_EQ(sendBurst(sender, receiver, address), DATAGRAMS) << "a datagram wasn't sent or didn't arrive: " << WSAGetLastError();
        constexpr std::uint64_t BURST_BYTES = static_cast<std::uint64_t>(DATAGRAMS) * DATAGRAM_BYTES;
        const std::uint64_t sentTarget = before.bytesSent + BURST_BYTES;
        const std::uint64_t receivedTarget = before.bytesReceived + BURST_BYTES;
        Platform::UdpProcessTraffic after = before;
        const auto countedBy = std::chrono::steady_clock::now() + 15s;
        while ((after.bytesSent < sentTarget || after.bytesReceived < receivedTarget) && std::chrono::steady_clock::now() < countedBy)
        {
            std::this_thread::sleep_for(250ms);
            const std::optional<Platform::UdpProcessTraffic> now = ownTraffic(collector.sample());
            ASSERT_TRUE(now.has_value()) << "this process dropped out of the UDP sample: the consumer stopped";
            after = now.value_or(Platform::UdpProcessTraffic{});
        }
        EXPECT_GE(after.bytesSent, sentTarget) << "baseline " << before.bytesSent;
        EXPECT_GE(after.bytesReceived, receivedTarget) << "baseline " << before.bytesReceived;
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

/// The first process (not PID 0 or 4) that IsProcessCritical calls critical and that this process can open for a
/// priority change, as setPriority() does: winlogon.exe on a client Windows, the DcomLaunch svchost.exe and others.
/// A protected one (csrss.exe) refuses the open, so the guard would never be reached on it. nullopt when none is.
[[nodiscard]] std::optional<Platform::ProcessTarget> openableCriticalProcess()
{
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE; more = Process32NextW(snapshot.get(), &entry))
    {
        constexpr DWORD SYSTEM_PID = 4;
        if (entry.th32ProcessID == 0 || entry.th32ProcessID == SYSTEM_PID)
        {
            continue;
        }
        const UniqueHandle process(OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        BOOL critical = FALSE;
        if (process && IsProcessCritical(process.get(), &critical) != FALSE && critical != FALSE)
        {
            return Platform::ProcessTarget{
                .pid = static_cast<std::int32_t>(entry.th32ProcessID),
                .startTimeTicks = startTicksOf(process.get()),
            };
        }
    }
    return std::nullopt;
}

TEST_F(CiRealSystemCriticalProcessTest, ACriticalProcessIsRefusedAPriorityChange)
{
    // Read-only: the real IsProcessCritical answers on csrss.exe, and the guard refuses a priority change on a
    // critical process before its call, which is a fake here, so even a broken guard changes nothing. Which
    // processes are critical depends on the edition (winlogon.exe isn't on the windows-2025 runner), so the test
    // takes the first critical one it may open.
    const DebugPrivilegeRestorer restorer;
    ASSERT_EQ(Platform::Windows::enableDebugPrivilegeIfElevated(), Platform::Windows::DebugPrivilegeResult::Enabled);
    const std::vector<DWORD> csrss = pidsNamed(L"csrss.exe");
    ASSERT_FALSE(csrss.empty()) << "no csrss.exe runs";
    const UniqueHandle session(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, csrss.front()));
    ASSERT_TRUE(session) << GetLastError();
    BOOL csrssCritical = FALSE;
    ASSERT_NE(IsProcessCritical(session.get(), &csrssCritical), FALSE) << GetLastError();
    EXPECT_NE(csrssCritical, FALSE) << "csrss.exe isn't critical";

    const std::optional<Platform::ProcessTarget> found = openableCriticalProcess();
    ASSERT_TRUE(found.has_value()) << "no critical process this administrator, with SeDebugPrivilege, may open";
    const Platform::ProcessTarget target = found.value_or(Platform::ProcessTarget{});
    RecordProperty("criticalPid", std::to_string(target.pid));

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
    EXPECT_EQ(priorityCalls().load(), 0) << "the guard let a priority change on critical process " << target.pid << " through";
}

// --- #3364: a 32-bit (WOW64) process's environment ----------------------------------------------------------

class CiRealSystemWow64EnvironmentTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemWow64EnvironmentTest, A32BitCmdsEnvironmentIsReadThroughItsPeb32)
{
    // SysWOW64's cmd.exe, idle at "pause" (its console has no window, so no key ever comes), started with a
    // variable it inherits; the reader finds it behind the 32-bit PEB. The Child ends it on scope exit.
    std::array<wchar_t, MAX_PATH> wow64{};
    const UINT length = GetSystemWow64DirectoryW(wow64.data(), static_cast<UINT>(wow64.size()));
    if (length == 0 || length >= wow64.size())
    {
        GTEST_SKIP() << "this Windows has no WOW64 (GetSystemWow64DirectoryW: " << GetLastError() << ")";
    }
    const std::filesystem::path cmd32 = std::filesystem::path(std::wstring(wow64.data(), length)) / L"cmd.exe";
    if (!std::filesystem::exists(cmd32))
    {
        GTEST_SKIP() << "no 32-bit cmd.exe at " << Core::pathToUtf8(cmd32);
    }
    constexpr std::string_view NAME = "TASKSMACK_CI_WOW64_ENVIRONMENT_3364";
    const std::wstring suffix = uniqueSuffix();
    std::optional<Child> started;
    {
        const ScopedVariable variable(L"TASKSMACK_CI_WOW64_ENVIRONMENT_3364", L"caf\u00E9 32-bit " + suffix);
        started.emplace(start(cmd32, {.parameters = L"/d /c pause"}));
    }
    const Child& child = *started;
    ASSERT_NE(child.process(), nullptr);
    USHORT machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
    ASSERT_NE(IsWow64Process2(child.process(), &machine, &native), FALSE) << GetLastError();
    ASSERT_EQ(machine, IMAGE_FILE_MACHINE_I386) << "SysWOW64's cmd.exe isn't running as x86";

    std::string want = "caf\xC3\xA9 32-bit ";
    for (const wchar_t c : suffix)
    {
        want += static_cast<char>(c); // hex digits: ASCII
    }
    Platform::Windows::WindowsProcessEnvironmentReader reader;
    Platform::EnvironmentReadResult result;
    std::optional<std::string> value;
    const auto deadline = std::chrono::steady_clock::now() + STARTUP_WAIT;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        ASSERT_FALSE(child.exitCode(0ms).has_value()) << "the 32-bit cmd.exe exited";
        result = reader.readEnvironment(child.target());
        const auto found = std::ranges::find(result.variables, NAME, &Platform::EnvironmentVariable::name);
        value = (found == result.variables.end()) ? std::nullopt : std::optional(found->value);
        if (value.has_value())
        {
            break;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    ASSERT_EQ(result.status, Platform::EnvironmentReadStatus::Ok) << result.detail;
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(value, std::optional<std::string>(want));
}

// --- The tests that skip only because the runner is elevated, run unelevated ----------------------------------

class CiRealSystemStandardUserTest : public TestSupport::CiRealSystemTest
{};

/// The tests that skip on an elevated runner, and pass unelevated.
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

/// Whether @p process's token reads as elevated (TokenElevation); nullopt when it can't be read.
[[nodiscard]] std::optional<bool> isElevated(HANDLE process)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token) == FALSE)
    {
        return std::nullopt;
    }
    const UniqueHandle owned(token);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (GetTokenInformation(owned.get(), TokenElevation, &elevation, sizeof(elevation), &size) == FALSE)
    {
        return std::nullopt;
    }
    return elevation.TokenIsElevated != 0;
}

/// Gives @p sid @p rights on file or folder @p path, keeping what is there, and takes them away again on scope exit:
/// what the throwaway account needs of the build's files and of the report's folder. A failure is added when the
/// grant can't be made.
class FileAccessGrant
{
  public:
    FileAccessGrant(std::filesystem::path path, std::vector<std::byte> sid, DWORD rights, DWORD inheritance = NO_INHERITANCE)
        : m_Path(std::move(path)), m_Sid(std::move(sid)), m_Granted(change(GRANT_ACCESS, rights, inheritance))
    {
        EXPECT_TRUE(m_Granted) << "couldn't let the throwaway account in to " << Core::pathToUtf8(m_Path);
    }
    ~FileAccessGrant()
    {
        try
        {
            if (m_Granted)
            {
                change(REVOKE_ACCESS, 0, NO_INHERITANCE);
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch) - out of memory: the ACE stays on the throwaway runner
        {}
    }
    FileAccessGrant(const FileAccessGrant&) = delete;
    FileAccessGrant& operator=(const FileAccessGrant&) = delete;
    FileAccessGrant(FileAccessGrant&&) = delete;
    FileAccessGrant& operator=(FileAccessGrant&&) = delete;

  private:
    bool change(ACCESS_MODE mode, DWORD rights, DWORD inheritance)
    {
        std::wstring name = m_Path.wstring();
        PACL existing = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (GetNamedSecurityInfoW(
                name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &existing, nullptr, &descriptor) !=
            ERROR_SUCCESS)
        {
            return false;
        }
        EXPLICIT_ACCESS_W entry{};
        entry.grfAccessPermissions = rights;
        entry.grfAccessMode = mode;
        entry.grfInheritance = inheritance;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        // TRUSTEE_IS_SID: ptstrName holds the SID, as the API documents.
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(m_Sid.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        PACL updated = nullptr;
        DWORD result = SetEntriesInAclW(1, &entry, existing, &updated);
        if (result == ERROR_SUCCESS)
        {
            result = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
        }
        LocalFree(static_cast<HLOCAL>(updated));
        LocalFree(descriptor);
        return result == ERROR_SUCCESS;
    }

    std::filesystem::path m_Path;
    std::vector<std::byte> m_Sid;
    bool m_Granted = false;
};

/// The programs and libraries beside @p exe (this binary, its DLLs, llvmpipe's opengl32.dll): what a run of it
/// loads from its own folder.
[[nodiscard]] std::vector<std::filesystem::path> loadedFromItsFolder(const std::filesystem::path& exe)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(exe.parent_path(), error))
    {
        const std::wstring extension = entry.path().extension().wstring();
        if (entry.is_regular_file(error) && (sameText(extension, L".exe") || sameText(extension, L".dll")))
        {
            files.push_back(entry.path());
        }
    }
    return files;
}

/// @p exe started with @p parameters in @p folder as @p account (CreateProcessWithLogonW, its profile loaded, so it
/// has a %TEMP% of its own), hidden, with that account's own environment: none of this process's variables, so not
/// the guard's either. An empty Child, with a failure added, when it can't be. The password goes only to the logon.
[[nodiscard]] Child startAs(const ThrowawayAccount& account,
                            const std::filesystem::path& exe,
                            const std::wstring& parameters,
                            const std::filesystem::path& folder)
{
    std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + parameters;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    if (CreateProcessWithLogonW(account.name().c_str(),
                                L".",
                                account.password().c_str(),
                                LOGON_WITH_PROFILE,
                                exe.c_str(),
                                commandLine.data(),
                                0,
                                nullptr,
                                folder.c_str(),
                                &startup,
                                &info) == FALSE)
    {
        ADD_FAILURE() << "couldn't start " << Core::pathToUtf8(exe) << " as the throwaway account: " << GetLastError();
        return {};
    }
    return {info.hProcess, info.hThread, info.dwProcessId};
}

TEST_F(CiRealSystemStandardUserTest, TheElevatedOnlySkipsRunAsAStandardUser)
{
    // This binary again, running only those tests, as a throwaway local standard account. On the runner UAC is
    // off, so every token of the runner's own user is elevated, Explorer's too; and a token restricted by hand
    // (Administrators deny-only, Medium) still reads as elevated (TokenElevation), so those tests would skip again.
    // A standard account's logon token isn't elevated, UAC or not. Its JSON report says each ran and passed.
    const ThrowawayAccount account;
    ASSERT_TRUE(account.ready());
    const std::filesystem::path exe = ownExecutable();
    std::vector<std::unique_ptr<FileAccessGrant>> grants;
    for (const std::filesystem::path& file : loadedFromItsFolder(exe))
    {
        grants.push_back(std::make_unique<FileAccessGrant>(file, account.sid(), GENERIC_READ | GENERIC_EXECUTE));
    }
    const ScratchFolder folder(std::filesystem::temp_directory_path() / (L"TaskSmackCiStandardUser." + uniqueSuffix()));
    const FileAccessGrant reportAccess(folder.path(), account.sid(), GENERIC_ALL, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    const std::filesystem::path report = folder.path() / L"report.json";
    const std::wstring parameters =
        L"--gtest_filter=" + std::wstring(ELEVATED_ONLY_SKIPS) + L" \"--gtest_output=json:" + report.wstring() + L"\"";
    const Child child = startAs(account, exe, parameters, folder.path());
    ASSERT_NE(child.process(), nullptr);
    // The token is the account's, whose DACL may not let this administrator read it; the report says it either way.
    if (const std::optional<bool> elevated = isElevated(child.process()); elevated.has_value())
    {
        EXPECT_FALSE(*elevated) << "the child runs elevated";
    }
    const std::optional<DWORD> exit = child.exitCode(2 * STARTUP_WAIT);
    ASSERT_TRUE(exit.has_value()) << "the standard user's run didn't finish";
    std::ifstream in(report, std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(json.empty()) << "the standard user's run wrote no report; its exit status " << exit.value_or(0);
    EXPECT_EQ(exit.value_or(1), 0U) << json;
    EXPECT_EQ(occurrences(json, "\"result\": \"COMPLETED\""), 2U) << json;
    EXPECT_EQ(occurrences(json, "\"result\": \"SKIPPED\""), 0U) << json;
    EXPECT_EQ(occurrences(json, "\"failure\""), 0U) << json;
}

} // namespace
