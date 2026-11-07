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
#include <expected>
#include <filesystem>
#include <fstream>
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

/// The variables that hand the child run its dialog's title and starting folder; unset, the child test skips.
constexpr std::wstring_view FOLDER_DIALOG_TITLE_VARIABLE = L"TASKSMACK_CI_FOLDER_DIALOG_TITLE";
constexpr std::wstring_view FOLDER_DIALOG_START_VARIABLE = L"TASKSMACK_CI_FOLDER_DIALOG_START";
/// The child run's one test.
constexpr std::wstring_view FOLDER_DIALOG_CHILD_TEST = L"CiRealSystemFolderPickerChildTest.ShowsTheDialogUntilItIsAnswered";

/// @p name's value, as text; empty when it isn't set.
[[nodiscard]] std::wstring variable(std::wstring_view name)
{
    const std::wstring key(name);
    const DWORD needed = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (needed == 0)
    {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(key.c_str(), value.data(), needed);
    value.resize(length < needed ? length : 0);
    return value;
}

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

/// The visible dialog of @p child titled @p title, within @p timeout; null when there is none, or once the child
/// has exited.
[[nodiscard]] HWND dialogTitled(const std::wstring& title, const Child& child, std::chrono::milliseconds timeout)
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
        if (dialog != nullptr && owner == child.pid() && IsWindowVisible(dialog) != FALSE)
        {
            return dialog;
        }
        if (child.exitCode(0ms).has_value())
        {
            return nullptr;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return nullptr;
}

/// Run only as CiRealSystemFolderPickerTest's child: the modal dialog blocks until it is answered, so it is shown
/// in a process of its own, which the parent can end, and never in the process running the other tests.
class CiRealSystemFolderPickerChildTest : public TestSupport::CiRealSystemTest
{
  protected:
    void SetUp() override
    {
        TestSupport::CiRealSystemTest::SetUp();
        if (!IsSkipped() && variable(FOLDER_DIALOG_TITLE_VARIABLE).empty())
        {
            GTEST_SKIP() << "runs only as CiRealSystemFolderPickerTest's child, "
                            "which shows the dialog it answers";
        }
    }
};

TEST_F(CiRealSystemFolderPickerChildTest, ShowsTheDialogUntilItIsAnswered)
{
    // The real IFileOpenDialog (FOS_PICKFOLDERS), on a worker thread as the Processes tab runs it; the parent answers.
    const std::wstring title = variable(FOLDER_DIALOG_TITLE_VARIABLE);
    const std::filesystem::path initial(variable(FOLDER_DIALOG_START_VARIABLE));
    Platform::FolderChoice result = std::unexpected(std::string("the dialog's thread didn't run"));
    {
        // A raw jthread, not a Domain::Worker: one blocking call in a fresh STA, joined right here.
        const std::jthread picker([&result, &initial, &title] { result = Platform::Windows::pickFolder(nullptr, initial, title); });
    }
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_FALSE(result->has_value()) << "a cancelled dialog chose " << Core::pathToUtf8(result->value_or(std::filesystem::path()));
}

class CiRealSystemFolderPickerTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemFolderPickerTest, TheFolderDialogShowsAndIsCancelled)
{
    // This binary again, running only the child test, which shows the dialog. Cancel is pressed as a click on it
    // would (WM_COMMAND IDCANCEL to the dialog), so no input reaches another window. Nothing here waits on the modal
    // itself: the child is ended if it hasn't exited by the deadline, and its report says how it went.
    const ScratchFolder folder(std::filesystem::temp_directory_path() / (L"TaskSmackCiFolder." + uniqueSuffix()));
    const std::wstring title = L"TaskSmack CI folder dialog " + uniqueSuffix();
    const std::filesystem::path report = folder.path() / L"report.json";
    Child child;
    {
        const ScopedVariable titleVariable(std::wstring(FOLDER_DIALOG_TITLE_VARIABLE), title);
        const ScopedVariable startVariable(std::wstring(FOLDER_DIALOG_START_VARIABLE), folder.path().wstring());
        const std::wstring parameters =
            L"--gtest_filter=" + std::wstring(FOLDER_DIALOG_CHILD_TEST) + L" \"--gtest_output=json:" + report.wstring() + L"\"";
        child = start(ownExecutable(), {.parameters = parameters});
    }
    ASSERT_NE(child.process(), nullptr);

    HWND dialog = dialogTitled(title, child, STARTUP_WAIT);
    EXPECT_NE(dialog, nullptr) << "the folder dialog never showed (the child's exit status " << child.exitCode(0ms).value_or(STILL_ACTIVE)
                               << ")";
    if (dialog != nullptr)
    {
        PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    }
    std::optional<DWORD> exit = child.exitCode(CLOSE_WAIT);
    if (!exit.has_value())
    {
        ADD_FAILURE() << "the folder dialog didn't close on IDCANCEL; the child is ended";
        child.end();
        exit = child.exitCode(0ms);
    }
    std::ifstream in(report, std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(json.empty()) << "the child run wrote no report; its exit status " << exit.value_or(STILL_ACTIVE);
    EXPECT_EQ(exit.value_or(STILL_ACTIVE), 0U) << json;
    EXPECT_EQ(occurrences(json, "\"result\": \"COMPLETED\""), 1U) << json;
    EXPECT_EQ(occurrences(json, "\"failure\""), 0U) << json;
}

} // namespace
