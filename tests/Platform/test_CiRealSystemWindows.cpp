// CI real-system tests on Windows, phase 1 (#3489; tracker #3492): the real OS paths of Realtime priority
// (#3465), Run new task (#3471), Compressed memory (#3482) and Open file location (#3461), which until now had
// only fakes plus a manual check. They change the machine (a process at Realtime, programs started through the
// shell, an Explorer window), so they run only on a disposable GitHub-hosted runner, where the runner's user is
// an administrator and UAC is off: CiRealSystem.h's guard skips them everywhere else. Every child is idle (a
// suspended cmd.exe, or TaskSmackCiStub.exe, which only sleeps) and ends on scope exit.

#include "Core/Utf8Path.h"
#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/SystemTypes.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsRunProgram.h"
#include "Platform/Windows/WindowsSystemProbe.h"
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
#include <windows.h>
#include <pdh.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef TASKSMACK_CI_STUB_PATH
#error "TASKSMACK_CI_STUB_PATH must name TaskSmackCiStub.exe (tests/CMakeLists.txt)"
#endif

namespace
{

using namespace std::chrono_literals;
using namespace TestSupport::CiRealSystem;
using Platform::ProcessActionResult;
using Platform::Windows::UniqueHandle;

/// Realtime's nice value: what Process Details passes to setPriority() to choose it.
constexpr std::int32_t REALTIME_NICE = Domain::Priority::classNice(Domain::Priority::PriorityClass::Realtime);

/// A suspended cmd.exe, started with @p token (or this process's own), whose class a test changes: it never
/// runs, so Realtime takes no CPU from the runner.
[[nodiscard]] Child suspendedCmd(HANDLE token = nullptr)
{
    return start(systemCmd(), {.token = token, .suspended = true, .parameters = L"/c exit 0"});
}

// --- #3465: Realtime priority --------------------------------------------------------------------------------

class CiRealSystemRealtimeTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemRealtimeTest, AnAdministratorSetsRealtimeAndWindowsKeepsIt)
{
    const Child child = suspendedCmd();
    ASSERT_NE(child.process(), nullptr);
    Platform::WindowsProcessActions actions;
    const ProcessActionResult result = actions.setPriority(child.target(), REALTIME_NICE);
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(GetPriorityClass(child.process()), static_cast<DWORD>(REALTIME_PRIORITY_CLASS));
}

/// Impersonates @p token on this thread until scope exit.
class Impersonating
{
  public:
    explicit Impersonating(HANDLE token) : m_Active(SetThreadToken(nullptr, token) != FALSE)
    {
        EXPECT_TRUE(m_Active) << "couldn't impersonate: " << GetLastError();
    }
    ~Impersonating()
    {
        if (m_Active)
        {
            RevertToSelf();
        }
    }
    Impersonating(const Impersonating&) = delete;
    Impersonating& operator=(const Impersonating&) = delete;
    Impersonating(Impersonating&&) = delete;
    Impersonating& operator=(Impersonating&&) = delete;

    [[nodiscard]] bool active() const noexcept
    {
        return m_Active;
    }

  private:
    bool m_Active;
};

TEST_F(CiRealSystemRealtimeTest, AStandardUserGetsHighAndTheReadBackSaysSo)
{
    // A standard user's TaskSmack: the child is its own (started with that token), and the call is made as that
    // user (impersonating it), who has no SeIncreaseBasePriorityPrivilege. SetPriorityClass succeeds, Windows sets
    // High, and only the class read back tells (#3429).
    const UniqueHandle token = standardUserPrimaryToken();
    ASSERT_TRUE(token);
    const Child child = suspendedCmd(token.get());
    ASSERT_NE(child.process(), nullptr);
    const UniqueHandle impersonation = impersonationOf(token.get());
    ASSERT_TRUE(impersonation);
    Platform::WindowsProcessActions actions;
    ProcessActionResult result;
    {
        const Impersonating asStandardUser(impersonation.get());
        ASSERT_TRUE(asStandardUser.active());
        result = actions.setPriority(child.target(), REALTIME_NICE);
    }
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.permissionDenied) << "the elevated helper is offered for it";
    EXPECT_EQ(result.errorMessage, Platform::REALTIME_NEEDS_ADMIN_MESSAGE);
    EXPECT_EQ(GetPriorityClass(child.process()), static_cast<DWORD>(HIGH_PRIORITY_CLASS));
}

// --- #3471: Run new task -------------------------------------------------------------------------------------

class CiRealSystemRunTaskTest : public TestSupport::CiRealSystemTest
{};

/// TaskSmackCiStub.exe copied into a folder of its own, so the process the shell starts is told apart by its path.
class StubCopy
{
  public:
    explicit StubCopy(const std::wstring& folderName)
        : m_Folder(std::filesystem::temp_directory_path() / (folderName + L"." + uniqueSuffix()))
    {
        copyFile(TASKSMACK_CI_STUB_PATH, exe());
    }
    [[nodiscard]] std::filesystem::path exe() const
    {
        return m_Folder.path() / L"TaskSmackCiStub.exe";
    }

  private:
    ScratchFolder m_Folder;
};

/// Run new task's command line for @p exe: the path quoted, then how long the stub waits.
[[nodiscard]] Platform::RunProgramRequest runRequest(const std::filesystem::path& exe, bool asAdministrator)
{
    return {.commandLine = std::format("\"{}\" 60000", Core::pathToUtf8(exe)), .asAdministrator = asAdministrator};
}

/// Whether UAC is on (EnableLUA), when a runas launch would raise its prompt on the secure desktop.
[[nodiscard]] bool uacIsOn()
{
    DWORD value = 1;
    auto size = static_cast<DWORD>(sizeof(value));
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
                                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                                        L"EnableLUA",
                                        RRF_RT_REG_DWORD,
                                        nullptr,
                                        &value,
                                        &size);
    return status != ERROR_SUCCESS || value != 0;
}

TEST_F(CiRealSystemRunTaskTest, OpenStartsTheProgram)
{
    const StubCopy stub(L"TaskSmackCiRunOpen");
    const ProcessActionResult result =
        Platform::Windows::runShellProgram(Platform::Windows::realRunProgramFunctions(), false, runRequest(stub.exe(), false));
    ASSERT_TRUE(result.success) << result.errorMessage;
    // ShellExecuteExW hands back no process (no SEE_MASK_NOCLOSEPROCESS), so it is found by its own path.
    const UniqueHandle started = findProcessRunning(stub.exe(), SYNCHRONIZE | PROCESS_TERMINATE, STARTUP_WAIT);
    ASSERT_TRUE(started) << "nothing runs " << Core::pathToUtf8(stub.exe());
    const EndOnExit ended(started.get());
    const DWORD pid = GetProcessId(started.get());
    EXPECT_NE(pid, 0U);
    RecordProperty("startedPid", std::to_string(pid));
}

TEST_F(CiRealSystemRunTaskTest, RunasStartsItElevated)
{
    if (uacIsOn())
    {
        GTEST_SKIP() << "UAC is on here, so runas would raise its consent prompt, which nothing can answer";
    }
    const StubCopy stub(L"TaskSmackCiRunAs");
    // Not elevated as far as runShellProgram() knows, so it asks for the runas verb, which with UAC off starts
    // the program with the administrator's full token and no prompt.
    const ProcessActionResult result =
        Platform::Windows::runShellProgram(Platform::Windows::realRunProgramFunctions(), false, runRequest(stub.exe(), true));
    ASSERT_TRUE(result.success) << result.errorMessage;
    const UniqueHandle started = findProcessRunning(stub.exe(), SYNCHRONIZE | PROCESS_TERMINATE, STARTUP_WAIT);
    ASSERT_TRUE(started) << "nothing runs " << Core::pathToUtf8(stub.exe());
    const EndOnExit ended(started.get());
    HANDLE token = nullptr;
    ASSERT_NE(OpenProcessToken(started.get(), TOKEN_QUERY, &token), FALSE) << GetLastError();
    const UniqueHandle startedToken(token);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    ASSERT_NE(GetTokenInformation(startedToken.get(), TokenElevation, &elevation, sizeof(elevation), &size), FALSE) << GetLastError();
    EXPECT_NE(elevation.TokenIsElevated, 0U);
}

/// The top-level windows this process owns: a shell error dialog would be one.
[[nodiscard]] std::vector<HWND> ownTopLevelWindows()
{
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL
        {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == GetCurrentProcessId())
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr) - EnumWindows' context
                reinterpret_cast<std::vector<HWND>*>(param)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return windows;
}

TEST_F(CiRealSystemRunTaskTest, AMisspelledNameSaysSoWithNoDialog)
{
    const std::vector<HWND> before = ownTopLevelWindows();
    const std::string name = std::format("TaskSmackNoSuchProgram{}", GetCurrentProcessId());
    const ProcessActionResult result = Platform::Windows::runShellProgram(
        Platform::Windows::realRunProgramFunctions(), false, {.commandLine = name, .asAdministrator = false});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.errorMessage, std::format("Windows can't find {}. Check the name and try again", name));
    // SEE_MASK_FLAG_NO_UI: the shell showed no "Windows cannot find" box of its own; TaskSmack shows the error.
    std::vector<HWND> after = ownTopLevelWindows();
    std::erase_if(after, [&before](HWND window) { return std::ranges::find(before, window) != before.end(); });
    EXPECT_TRUE(after.empty()) << after.size() << " new window(s) of this process";
    for (HWND window : after)
    {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

// --- #3482: Compressed memory --------------------------------------------------------------------------------

class CiRealSystemMemoryCompressionTest : public TestSupport::CiRealSystemTest
{};

/// The Memory Compression process's working set, read without the probe: GetProcessMemoryInfo on the process
/// Toolhelp names, else PDH's Process(Memory Compression)\Working Set. nullopt when neither can read it.
[[nodiscard]] std::optional<std::uint64_t> compressionWorkingSet(DWORD pid)
{
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (process && GetProcessMemoryInfo(process.get(), &counters, sizeof(counters)) != FALSE)
    {
        return counters.WorkingSetSize;
    }
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    std::optional<std::uint64_t> value;
    PDH_HCOUNTER counter = nullptr;
    PDH_FMT_COUNTERVALUE formatted{};
    if (PdhAddEnglishCounterW(query, L"\\Process(Memory Compression)\\Working Set", 0, &counter) == ERROR_SUCCESS &&
        PdhCollectQueryData(query) == ERROR_SUCCESS &&
        PdhGetFormattedCounterValue(counter, PDH_FMT_LARGE, nullptr, &formatted) == ERROR_SUCCESS)
    {
        value = static_cast<std::uint64_t>(
            formatted.largeValue); // NOLINT(cppcoreguidelines-pro-type-union-access) - PDH_FMT_LARGE fills largeValue
    }
    PdhCloseQuery(query);
    return value;
}

TEST_F(CiRealSystemMemoryCompressionTest, TheProbeReadsTheCompressionStoresWorkingSet)
{
    const std::vector<DWORD> found = pidsNamed(L"Memory Compression");
    Platform::WindowsSystemProbe probe;
    const Platform::SystemCounters first = probe.read();
    if (found.empty())
    {
        EXPECT_FALSE(first.memory.hasMemoryCompression) << "the probe found a Memory Compression process Toolhelp doesn't list";
        GTEST_SKIP() << "memory compression is off on this runner: there is no Memory Compression process (Server images "
                        "leave it off; Enable-MMAgent -MemoryCompression and a reboot turn it on)";
    }
    ASSERT_EQ(found.size(), 1U);
    ASSERT_TRUE(first.memory.hasMemoryCompression)
        << "Memory Compression runs as PID " << found.front() << ", but the probe didn't find it";
    const std::optional<std::uint64_t> independent = compressionWorkingSet(found.front());
    const Platform::SystemCounters second = probe.read();
    ASSERT_TRUE(independent.has_value()) << "neither GetProcessMemoryInfo nor PDH read PID " << found.front();
    const std::uint64_t independentBytes = independent.value_or(0);
    ASSERT_TRUE(second.memory.hasMemoryCompression);
    // The store grows and shrinks between reads; the independent figure, read between the probe's two, lies
    // within their range, give or take a quarter and 16 MiB.
    constexpr std::uint64_t SLACK_BYTES = 16ULL * 1024 * 1024;
    const std::uint64_t low = std::min(first.memory.memoryCompressionBytes, second.memory.memoryCompressionBytes);
    const std::uint64_t high = std::max(first.memory.memoryCompressionBytes, second.memory.memoryCompressionBytes);
    EXPECT_GE(independentBytes + SLACK_BYTES, low - (low / 4)) << "probe " << low << ".." << high << ", independent " << independentBytes;
    EXPECT_LE(independentBytes, high + (high / 4) + SLACK_BYTES) << "probe " << low << ".." << high << ", independent " << independentBytes;
    RecordProperty("compressionBytes", std::to_string(independentBytes));
}

// --- #3461: Open file location -------------------------------------------------------------------------------

class CiRealSystemFileLocationTest : public TestSupport::CiRealSystemTest
{};

/// Explorer's folder windows (CabinetWClass) on this desktop.
[[nodiscard]] std::vector<HWND> explorerWindows()
{
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL
        {
            std::array<wchar_t, 64> name{};
            const int length = GetClassNameW(window, name.data(), static_cast<int>(name.size()));
            if (std::wstring_view(name.data(), static_cast<std::size_t>(std::max(length, 0))) == L"CabinetWClass")
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr) - EnumWindows' context
                reinterpret_cast<std::vector<HWND>*>(param)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return windows;
}

[[nodiscard]] std::wstring titleOf(HWND window)
{
    std::array<wchar_t, 1024> title{};
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    return {title.data(), static_cast<std::size_t>(std::max(length, 0))};
}

/// Open file location on TaskSmackCiStub.exe copied into a folder named @p folderName: Explorer's new window
/// shows that folder (its title is the folder's name), and is closed.
void expectExplorerShowsTheFolder(const std::wstring& folderName)
{
    const ScratchFolder folder(std::filesystem::temp_directory_path() / folderName);
    const std::filesystem::path exe = folder.path() / L"TaskSmackCiStub.exe";
    copyFile(TASKSMACK_CI_STUB_PATH, exe);
    const Child stub = start(exe, {.parameters = L"60000"});
    ASSERT_NE(stub.process(), nullptr);
    const bool hadShell = GetShellWindow() != nullptr;
    const std::vector<HWND> before = explorerWindows();

    Platform::WindowsProcessActions actions;
    const ProcessActionResult result = actions.openFileLocation(stub.target());
    ASSERT_TRUE(result.success) << result.errorMessage;

    HWND shown = nullptr;
    std::wstring lastTitle;
    const auto deadline = std::chrono::steady_clock::now() + STARTUP_WAIT;
    while (shown == nullptr && std::chrono::steady_clock::now() < deadline)
    {
        for (HWND window : explorerWindows())
        {
            if (std::ranges::find(before, window) != before.end())
            {
                continue;
            }
            lastTitle = titleOf(window);
            if (lastTitle.contains(folderName))
            {
                shown = window;
                break;
            }
        }
        std::this_thread::sleep_for(100ms);
    }
    if (shown == nullptr && !hadShell)
    {
        GTEST_SKIP() << "no Explorer window came up, and this runner's desktop has no shell (GetShellWindow() is null), "
                        "so Explorer can't show a folder here";
    }
    ASSERT_NE(shown, nullptr) << "no new Explorer window shows the folder; the last new one's title: "
                              << Core::pathToUtf8(std::filesystem::path(lastTitle));
    EXPECT_NE(PostMessageW(shown, WM_CLOSE, 0, 0), FALSE) << GetLastError();
    const auto closeDeadline = std::chrono::steady_clock::now() + 10s;
    while (IsWindow(shown) != FALSE && std::chrono::steady_clock::now() < closeDeadline)
    {
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(IsWindow(shown), FALSE) << "the Explorer window didn't close";
}

TEST_F(CiRealSystemFileLocationTest, ExplorerShowsTheExecutablesFolder)
{
    expectExplorerShowsTheFolder(L"TaskSmackCiOpenLocation." + uniqueSuffix());
}

TEST_F(CiRealSystemFileLocationTest, ExplorerShowsAFolderWithSpacesAndNonAsciiInItsName)
{
    // Spaces, accented Latin, CJK and an emoji (a surrogate pair): each breaks a narrow or unquoted path.
    expectExplorerShowsTheFolder(L"TaskSmack CI Ünïcödé 任务 \U0001F525 " + uniqueSuffix());
}

} // namespace
