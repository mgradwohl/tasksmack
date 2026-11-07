// CI real-system tests on Windows, phase 3 (#3490; tracker #3492): TaskSmack.exe itself started on the runner. The
// sign-in entry's --minimized start shows the window minimized without taking the foreground (#3093 part 2, never
// launched before: #3259); a second start in Task Manager's place brings the running one back and exits
// (showRunningTaskSmack(), #3072); and the folder dialog behind "Save dump to..." comes up and is cancelled (#2001).
// They open windows on the desktop and start the app, so they run only on a disposable GitHub-hosted runner, where
// CiRealSystem.h's guard lets them. The app runs from a scratch copy, with Mesa's software OpenGL beside it as the
// display suites have theirs, its own TASKSMACK_CONFIG_DIR and TEMP, and is closed with WM_CLOSE; a scope guard
// ends it only if it doesn't exit in time.
//
// Left manual: the title bar's native system menu (TitleBarLayer::showNativeSystemMenu()). It opens from a click on
// the title-bar icon, which the runner can reach only with real input, and real input goes to whichever window has
// the foreground, which a test can't promise on the runner; in a test binary it needs a running Core::Application.

#include "App/InstanceLock.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Utf8Path.h"
#include "Platform/IProcessActions.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsFolderPicker.h"
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
// clang-format on

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifndef TASKSMACK_APP_PATH
#error "TASKSMACK_APP_PATH must name TaskSmack.exe (tests/CMakeLists.txt)"
#endif

namespace
{

using namespace TestSupport::CiRealSystem;
using namespace std::chrono_literals;

/// How long the app has to exit once it was asked to close, or to bring the running window back.
constexpr std::chrono::milliseconds CLOSE_WAIT{30'000};
/// How long a window that started minimized is watched for anything that restores or activates it.
constexpr std::chrono::milliseconds SETTLE{2'000};

// --- A scratch copy of TaskSmack.exe, and its environment ---------------------------------------------------

/// TaskSmack.exe, the libraries beside it and its assets folder, copied into @p folder, with Mesa's software
/// OpenGL (opengl32.dll and libgallium_wgl.dll, which CI puts beside the test executables) beside the copy: the
/// runner has no GPU, and its own OpenGL is below the app's 3.3 core. The copy's path.
[[nodiscard]] std::filesystem::path copyTaskSmackInto(const std::filesystem::path& folder)
{
    const std::filesystem::path app = std::filesystem::path(TASKSMACK_APP_PATH).make_preferred();
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(app.parent_path(), error))
    {
        const std::wstring extension = entry.path().extension().wstring();
        if (entry.is_regular_file() && (sameText(extension, L".exe") || sameText(extension, L".dll")))
        {
            copyFile(entry.path(), folder / entry.path().filename());
        }
    }
    EXPECT_FALSE(error) << "couldn't list " << Core::pathToUtf8(app.parent_path()) << ": " << error.message();
    const std::filesystem::path assets = app.parent_path() / L"assets";
    std::filesystem::copy(assets, folder / L"assets", std::filesystem::copy_options::recursive, error);
    EXPECT_FALSE(error) << "couldn't copy " << Core::pathToUtf8(assets) << ": " << error.message();
    constexpr std::array<std::wstring_view, 2> MESA{L"opengl32.dll", L"libgallium_wgl.dll"};
    for (const std::wstring_view mesa : MESA)
    {
        const std::filesystem::path dll = ownExecutable().parent_path() / mesa;
        if (std::filesystem::exists(dll, error))
        {
            copyFile(dll, folder / mesa);
        }
    }
    return folder / app.filename();
}

/// A scratch TaskSmack: the copy, a config folder and a TEMP of its own, all removed on scope exit.
class ScratchTaskSmack
{
  public:
    ScratchTaskSmack()
        : m_Root(std::filesystem::temp_directory_path() / (L"TaskSmackCiApp." + uniqueSuffix())),
          m_Install(m_Root.path() / L"app"),
          m_Config(m_Root.path() / L"config"),
          m_Temp(m_Root.path() / L"temp"),
          m_Exe(copyTaskSmackInto(m_Install.path()))
    {}

    [[nodiscard]] const std::filesystem::path& exe() const noexcept
    {
        return m_Exe;
    }

    /// The copy started with @p parameters, in its own config folder and TEMP. The variables are set only for the
    /// start: a child takes its environment when it is made.
    [[nodiscard]] Child start(const std::wstring& parameters) const
    {
        const ScopedVariable config(Platform::WinString::utf8ToWide(Core::ConfigDirOverride::ENV_VAR), m_Config.path().wstring());
        const ScopedVariable temp(L"TEMP", m_Temp.path().wstring());
        const ScopedVariable tmp(L"TMP", m_Temp.path().wstring());
        const ScopedVariable gallium(L"GALLIUM_DRIVER", L"llvmpipe");
        return TestSupport::CiRealSystem::start(m_Exe, {.parameters = parameters});
    }

    /// The end of the app's log (tasksmack-debug.log in its TEMP), for a failure's message.
    [[nodiscard]] std::string logTail() const
    {
        constexpr std::size_t TAIL = 4'000;
        std::ifstream in(m_Temp.path() / L"tasksmack-debug.log", std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (text.size() > TAIL)
        {
            text.erase(0, text.size() - TAIL);
        }
        return "\n--- the end of its tasksmack-debug.log ---\n" + text;
    }

  private:
    ScratchFolder m_Root;
    ScratchFolder m_Install;
    ScratchFolder m_Config;
    ScratchFolder m_Temp;
    std::filesystem::path m_Exe;
};

// --- Its window -------------------------------------------------------------------------------------------------

/// Whether @p window is a visible, unowned top-level window of process @p pid titled "TaskSmack", or "TaskSmack
/// (Administrator)" elevated, as the runner is: the app's own window, as showRunningTaskSmack() looks for it.
[[nodiscard]] bool isTaskSmackWindowOf(HWND window, DWORD pid)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != pid || IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr)
    {
        return false;
    }
    std::array<wchar_t, 64> title{};
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    const std::wstring_view text(title.data(), static_cast<std::size_t>(length > 0 ? length : 0));
    return text == L"TaskSmack" || text.starts_with(L"TaskSmack (");
}

/// @p app's visible TaskSmack window, within @p timeout; null when it shows none, or exits first.
[[nodiscard]] HWND taskSmackWindowOf(const Child& app, std::chrono::milliseconds timeout)
{
    struct Search
    {
        DWORD pid = 0;
        HWND found = nullptr;
    };
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        Search search{.pid = app.pid()};
        EnumWindows(
            [](HWND window, LPARAM param) -> BOOL
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr) - EnumWindows' context
                auto* wanted = reinterpret_cast<Search*>(param);
                if (isTaskSmackWindowOf(window, wanted->pid))
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
        if (app.exitCode(0ms).has_value())
        {
            return nullptr;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return nullptr;
}

/// Whether @p window is minimized (or, with @p minimized false, not) within @p timeout.
[[nodiscard]] bool becomesIconic(HWND window, bool minimized, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        if ((IsIconic(window) != FALSE) == minimized)
        {
            return true;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return false;
}

/// Whether @p window stays minimized and out of the foreground for @p period.
[[nodiscard]] bool staysMinimizedInTheBackground(HWND window, std::chrono::milliseconds period)
{
    const auto deadline = std::chrono::steady_clock::now() + period;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        if (IsIconic(window) == FALSE || GetForegroundWindow() == window)
        {
            return false;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return true;
}

/// Asks @p app to close as the title bar's Close does (WM_CLOSE to @p window) and returns its exit status, once it
/// has exited within CLOSE_WAIT; nullopt while it still runs (the Child then ends it on scope exit).
[[nodiscard]] std::optional<DWORD> closeGracefully(const Child& app, HWND window)
{
    PostMessageW(window, WM_CLOSE, 0, 0);
    return app.exitCode(CLOSE_WAIT);
}

class CiRealSystemAppLaunchTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemAppLaunchTest, MinimizedStartsMinimizedWithoutTakingTheForeground)
{
    // The sign-in entry's start (#3093 part 2): "<TaskSmack.exe>" --minimized, created hidden, placed, then shown
    // straight to the taskbar with SW_SHOWMINNOACTIVE (Window::showMinimizedWithoutFocus()).
    const ScratchTaskSmack taskSmack;
    ASSERT_TRUE(std::filesystem::exists(taskSmack.exe())) << Core::pathToUtf8(taskSmack.exe());
    const Child app = taskSmack.start(L"--minimized");
    ASSERT_NE(app.process(), nullptr);

    HWND window = taskSmackWindowOf(app, STARTUP_WAIT);
    ASSERT_NE(window, nullptr) << "TaskSmack showed no window (exit " << app.exitCode(0ms).value_or(STILL_ACTIVE) << ")"
                               << taskSmack.logTail();
    EXPECT_NE(IsIconic(window), FALSE) << "its first visible state isn't minimized" << taskSmack.logTail();
    EXPECT_NE(GetForegroundWindow(), window) << "it took the foreground" << taskSmack.logTail();
    EXPECT_TRUE(staysMinimizedInTheBackground(window, SETTLE)) << "it was restored or activated after it started" << taskSmack.logTail();

    const std::optional<DWORD> exitCode = closeGracefully(app, window);
    ASSERT_TRUE(exitCode.has_value()) << "it didn't exit within " << CLOSE_WAIT.count() << " ms of WM_CLOSE" << taskSmack.logTail();
    EXPECT_EQ(exitCode.value_or(STILL_ACTIVE), static_cast<DWORD>(EXIT_SUCCESS)) << taskSmack.logTail();
}

TEST_F(CiRealSystemAppLaunchTest, ASecondStartInTaskManagersPlaceShowsTheRunningOneAndExits)
{
    // Ctrl+Shift+Esc with TaskSmack in Task Manager's place (#3072): Windows starts "<TaskSmack.exe>"
    // "<...\taskmgr.exe>", which finds this config folder's instance lock taken, so showRunningTaskSmack() restores
    // the running window and the second start exits with ALREADY_RUNNING_EXIT_CODE, without a message box.
    const ScratchTaskSmack taskSmack;
    ASSERT_TRUE(std::filesystem::exists(taskSmack.exe())) << Core::pathToUtf8(taskSmack.exe());
    const Child first = taskSmack.start(L"--minimized");
    ASSERT_NE(first.process(), nullptr);
    HWND window = taskSmackWindowOf(first, STARTUP_WAIT);
    ASSERT_NE(window, nullptr) << "the first TaskSmack showed no window" << taskSmack.logTail();
    ASSERT_TRUE(becomesIconic(window, true, STARTUP_WAIT)) << "the first TaskSmack didn't start minimized" << taskSmack.logTail();

    const std::filesystem::path taskManager = systemCmd().parent_path() / L"taskmgr.exe";
    const Child second = taskSmack.start(L"\"" + taskManager.wstring() + L"\"");
    ASSERT_NE(second.process(), nullptr);
    const std::optional<DWORD> secondExit = second.exitCode(STARTUP_WAIT);
    ASSERT_TRUE(secondExit.has_value()) << "the second TaskSmack didn't exit: a message box, or it found no window to show"
                                        << taskSmack.logTail();
    EXPECT_EQ(secondExit.value_or(STILL_ACTIVE), static_cast<DWORD>(App::ALREADY_RUNNING_EXIT_CODE));

    EXPECT_TRUE(becomesIconic(window, false, CLOSE_WAIT)) << "the running TaskSmack's window wasn't restored" << taskSmack.logTail();
    EXPECT_NE(IsWindowVisible(window), FALSE);
    EXPECT_FALSE(first.exitCode(0ms).has_value()) << "the running TaskSmack exited" << taskSmack.logTail();

    const std::optional<DWORD> firstExit = closeGracefully(first, window);
    ASSERT_TRUE(firstExit.has_value()) << "it didn't exit within " << CLOSE_WAIT.count() << " ms of WM_CLOSE" << taskSmack.logTail();
    EXPECT_EQ(firstExit.value_or(STILL_ACTIVE), static_cast<DWORD>(EXIT_SUCCESS)) << taskSmack.logTail();
}

// --- The folder dialog (#2001) -----------------------------------------------------------------------------------

class CiRealSystemFolderPickerTest : public TestSupport::CiRealSystemTest
{};

/// The visible dialog of this process titled @p title, within @p timeout; null when there is none, or once
/// @p done (the call that shows it) has returned.
[[nodiscard]] HWND
dialogTitled(const std::wstring& title, const std::future<Platform::FolderChoice>& done, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        HWND dialog = FindWindowW(L"#32770", title.c_str());
        DWORD owner = 0;
        if (dialog != nullptr)
        {
            GetWindowThreadProcessId(dialog, &owner);
        }
        if (dialog != nullptr && owner == GetCurrentProcessId() && IsWindowVisible(dialog) != FALSE)
        {
            return dialog;
        }
        if (done.wait_for(0ms) == std::future_status::ready)
        {
            return nullptr;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return nullptr;
}

TEST_F(CiRealSystemFolderPickerTest, TheFolderDialogShowsAndIsCancelled)
{
    // The real IFileOpenDialog (FOS_PICKFOLDERS), on a worker thread as the Processes tab runs it. Cancel is
    // pressed as a click on it would (WM_COMMAND IDCANCEL to the dialog), so no input reaches another window.
    const ScratchFolder initial(std::filesystem::temp_directory_path() / (L"TaskSmackCiFolder." + uniqueSuffix()));
    const std::wstring title = L"TaskSmack CI folder dialog " + uniqueSuffix();
    std::promise<Platform::FolderChoice> chosen;
    std::future<Platform::FolderChoice> done = chosen.get_future();
    // A raw jthread, not a Domain::Worker: one blocking call in a fresh STA, while this thread watches for its window.
    const std::jthread picker([&chosen, &initial, &title]
                              { chosen.set_value(Platform::Windows::pickFolder(nullptr, initial.path(), title)); });

    HWND dialog = dialogTitled(title, done, STARTUP_WAIT);
    EXPECT_NE(dialog, nullptr) << "the folder dialog never showed";
    if (dialog != nullptr)
    {
        PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    }
    // However it went, the worker must return before the jthread joins: close whatever is still up.
    for (int attempt = 0; attempt < 10 && done.wait_for(CLOSE_WAIT / 10) != std::future_status::ready; ++attempt)
    {
        if (HWND still = FindWindowW(L"#32770", title.c_str()); still != nullptr)
        {
            PostMessageW(still, WM_CLOSE, 0, 0);
        }
    }
    ASSERT_EQ(done.wait_for(CLOSE_WAIT), std::future_status::ready) << "the folder dialog didn't close";
    const Platform::FolderChoice result = done.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_FALSE(result->has_value()) << "a cancelled dialog chose " << Core::pathToUtf8(result->value_or(std::filesystem::path()));
}

} // namespace
