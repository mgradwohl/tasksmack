// CI real-system tests on Windows, phase 2 (#3490; tracker #3492): the registry paths that until now had only a
// scratch HKCU key, fakes and a manual check. Replace Task Manager writes the real HKLM IFEO key of taskmgr.exe
// (#3247, #3320 and #3305 never wrote it), Start at sign-in the real HKCU Run value (#3093), and the Startup tab's
// enable and disable the real StartupApproved values under HKCU and HKLM (#801). They change the machine, so they
// run only on a disposable GitHub-hosted runner, elevated, where CiRealSystem.h's guard lets them; every key and
// value they make is removed on scope exit, and a test skips rather than touch a value that was there before it.

#include "App/LaunchOptions.h"
#include "Core/Utf8Path.h"
#include "Domain/StartAtLogin.h"
#include "Platform/IProcessActions.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/TaskManagerReplacement.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsStartAtLogin.h"
#include "Platform/Windows/WindowsStartupActions.h"
#include "Platform/Windows/WindowsStartupProbe.h"
#include "Platform/Windows/WindowsStartupProbeMath.h"
#include "Platform/Windows/WindowsTaskManagerInstall.h"
#include "Platform/Windows/WindowsTaskManagerReplacement.h"
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
#include <shellapi.h>
// clang-format on

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef TASKSMACK_CI_STUB_PATH
#error "TASKSMACK_CI_STUB_PATH must name TaskSmackCiStub.exe (tests/CMakeLists.txt)"
#endif

namespace
{

using namespace TestSupport::CiRealSystem;
using Platform::ProcessActionResult;
namespace Replacement = Platform::TaskManagerReplacement;
namespace WinString = Platform::WinString;

// --- Registry reads and writes of the tests' own, in the 64-bit view -----------------------------------------

/// @p name's REG_SZ under @p root\@p key; nullopt when the key or the value isn't there.
[[nodiscard]] std::optional<std::wstring> readString(HKEY root, const std::wstring& key, const wchar_t* name)
{
    DWORD bytes = 0;
    constexpr DWORD FLAGS = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    if (RegGetValueW(root, key.c_str(), name, FLAGS, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
    {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key.c_str(), name, FLAGS, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value;
}

/// @p name's REG_BINARY under @p root\@p key; empty when the key or the value isn't there.
[[nodiscard]] std::vector<std::uint8_t> readBinary(HKEY root, const std::wstring& key, const std::wstring& name)
{
    std::vector<std::uint8_t> value(64);
    auto bytes = static_cast<DWORD>(value.size());
    if (RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_REG_BINARY, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
    {
        return {};
    }
    value.resize(bytes);
    return value;
}

/// Whether @p root\@p key is there.
[[nodiscard]] bool keyExists(HKEY root, const std::wstring& key)
{
    HKEY opened = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &opened) != ERROR_SUCCESS)
    {
        return false;
    }
    RegCloseKey(opened);
    return true;
}

/// Writes @p data as @p name's REG_SZ under @p root\@p key, creating the key; a failure added when it can't.
void writeString(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& data)
{
    HKEY opened = nullptr;
    const LSTATUS created = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &opened, nullptr);
    ASSERT_EQ(created, ERROR_SUCCESS) << "couldn't create " << WinString::wideToUtf8(key);
    const auto bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the registry takes bytes
    const LSTATUS set = RegSetValueExW(opened, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), bytes);
    RegCloseKey(opened);
    EXPECT_EQ(set, ERROR_SUCCESS) << "couldn't write " << WinString::wideToUtf8(name);
}

/// Removes @p name under @p root\@p key, if it is there.
void deleteValue(HKEY root, const std::wstring& key, const std::wstring& name) noexcept
{
    HKEY opened = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &opened) == ERROR_SUCCESS)
    {
        RegDeleteValueW(opened, name.c_str());
        RegCloseKey(opened);
    }
}

/// Removes @p name under @p root\@p key on scope exit: a value a test made.
class ValueRemover
{
  public:
    ValueRemover(HKEY root, std::wstring key, std::wstring name) : m_Root(root), m_Key(std::move(key)), m_Name(std::move(name))
    {}
    ~ValueRemover()
    {
        deleteValue(m_Root, m_Key, m_Name);
    }
    ValueRemover(const ValueRemover&) = delete;
    ValueRemover& operator=(const ValueRemover&) = delete;
    ValueRemover(ValueRemover&&) = delete;
    ValueRemover& operator=(ValueRemover&&) = delete;

  private:
    HKEY m_Root;
    std::wstring m_Key;
    std::wstring m_Name;
};

// --- Replace Task Manager: the real HKLM IFEO key (#3072, #3280) ---------------------------------------------

/// The IFEO key every program's is under.
constexpr std::wstring_view IFEO_ROOT = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

/// taskmgr.exe's IFEO key, and the values TaskSmack writes in it, as the registry takes them.
[[nodiscard]] std::wstring taskManagerKey()
{
    return WinString::utf8ToWide(Replacement::IFEO_KEY);
}
[[nodiscard]] std::wstring debuggerValue()
{
    return WinString::utf8ToWide(Replacement::DEBUGGER_VALUE);
}
[[nodiscard]] std::wstring createdMarkerValue()
{
    return WinString::utf8ToWide(Replacement::CREATED_MARKER_VALUE);
}
/// taskmgr.exe's Debugger value; nullopt when there is none.
[[nodiscard]] std::optional<std::wstring> taskManagerDebugger()
{
    return readString(HKEY_LOCAL_MACHINE, taskManagerKey(), debuggerValue().c_str());
}

/// Puts taskmgr.exe's IFEO key back as it was on scope exit: no Debugger (a test that finds one skips), no
/// TaskSmack marker, and no key at all when there was none.
class TaskManagerKeyRestorer
{
  public:
    TaskManagerKeyRestorer() : m_KeyExisted(keyExists(HKEY_LOCAL_MACHINE, taskManagerKey()))
    {}
    ~TaskManagerKeyRestorer()
    {
        deleteValue(HKEY_LOCAL_MACHINE, taskManagerKey(), debuggerValue());
        deleteValue(HKEY_LOCAL_MACHINE, taskManagerKey(), createdMarkerValue());
        if (!m_KeyExisted)
        {
            RegDeleteKeyExW(HKEY_LOCAL_MACHINE, taskManagerKey().c_str(), KEY_WOW64_64KEY, 0);
        }
    }
    TaskManagerKeyRestorer(const TaskManagerKeyRestorer&) = delete;
    TaskManagerKeyRestorer& operator=(const TaskManagerKeyRestorer&) = delete;
    TaskManagerKeyRestorer(TaskManagerKeyRestorer&&) = delete;
    TaskManagerKeyRestorer& operator=(TaskManagerKeyRestorer&&) = delete;

    [[nodiscard]] bool keyExisted() const noexcept
    {
        return m_KeyExisted;
    }

  private:
    bool m_KeyExisted;
};

/// Another program's IFEO key, with a Debugger of its own, deleted on scope exit: what TaskSmack must never touch.
class OtherProgramsKey
{
  public:
    OtherProgramsKey() : m_Key(std::wstring(IFEO_ROOT) + L"\\TaskSmackCiOther" + uniqueSuffix() + L".exe")
    {
        writeString(HKEY_LOCAL_MACHINE, m_Key, debuggerValue(), std::wstring(debugger()));
    }
    ~OtherProgramsKey()
    {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, m_Key.c_str(), KEY_WOW64_64KEY, 0);
    }
    OtherProgramsKey(const OtherProgramsKey&) = delete;
    OtherProgramsKey& operator=(const OtherProgramsKey&) = delete;
    OtherProgramsKey(OtherProgramsKey&&) = delete;
    OtherProgramsKey& operator=(OtherProgramsKey&&) = delete;

    [[nodiscard]] static std::wstring_view debugger()
    {
        return L"\"C:\\Tools\\OtherDebugger.exe\" -x";
    }
    [[nodiscard]] std::optional<std::wstring> read() const
    {
        return readString(HKEY_LOCAL_MACHINE, m_Key, debuggerValue().c_str());
    }

  private:
    std::wstring m_Key;
};

class CiRealSystemTaskManagerTest : public TestSupport::CiRealSystemTest
{
  protected:
    void SetUp() override
    {
        TestSupport::CiRealSystemTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
        if (const auto existing = taskManagerDebugger())
        {
            GTEST_SKIP() << "taskmgr.exe already has a Debugger (" << WinString::wideToUtf8(*existing)
                         << "), which these tests won't overwrite";
        }
    }
};

/// System32's taskmgr.exe.
[[nodiscard]] std::filesystem::path systemTaskManager()
{
    return systemCmd().parent_path() / L"taskmgr.exe";
}

/// The arguments in @p commandLine, split as CommandLineToArgvW splits them, in UTF-8.
[[nodiscard]] std::vector<std::string> argumentsOf(const std::wstring& commandLine)
{
    std::vector<std::string> arguments;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &count);
    if (argv == nullptr)
    {
        return arguments;
    }
    for (const wchar_t* argument : std::span<LPWSTR>(argv, static_cast<std::size_t>(count)))
    {
        arguments.push_back(WinString::wideToUtf8(argument));
    }
    LocalFree(static_cast<HLOCAL>(argv));
    return arguments;
}

/// The text of @p file once it has some, within @p timeout; empty when it never does.
[[nodiscard]] std::string waitForText(const std::filesystem::path& file, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
    {
        std::ifstream in(file, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!text.empty())
        {
            return text;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return {};
}

TEST_F(CiRealSystemTaskManagerTest, TheElevatedRouteReplacesTaskManagerAndGivesItBack)
{
    // The elevated route (#3280): an elevated TaskSmack, installed where only an administrator can change it,
    // writes taskmgr.exe's Debugger itself. TaskSmackCiStub.exe stands in for that TaskSmack.exe. The runner has
    // UAC on, so this administrator's token is TokenElevationTypeFull and the check runs against its linked
    // token; UAC off (TokenElevationTypeDefault, the token restricted by hand) stays a manual check.
    const TaskManagerKeyRestorer restorer;
    const OtherProgramsKey other;
    const ScratchFolder install(programFiles() / (L"TaskSmackCiIfeo." + uniqueSuffix()));
    protectAsAnInstall(install.path());
    const std::filesystem::path exe = install.path() / L"TaskSmack.exe";
    copyFile(TASKSMACK_CI_STUB_PATH, exe);
    protectAsAnInstall(exe);
    const std::string self = WinString::wideToUtf8(Platform::Windows::BrokerHelper::finalPath(exe.wstring()).value_or(exe.wstring()));

    const std::vector<std::string> folders = Platform::Windows::installFolders();
    const auto direct = Platform::Windows::checkDirectWrite(self, folders);
    ASSERT_TRUE(direct.has_value()) << "the elevated route refuses " << self << ": " << direct.error();

    Platform::Windows::WindowsIfeoRegistry registry;
    const ProcessActionResult set = Replacement::applySet(registry, self, false);
    ASSERT_TRUE(set.success) << set.errorMessage;
    EXPECT_EQ(taskManagerDebugger(), WinString::utf8ToWide(Replacement::debuggerFor(self)));
    const auto readBack = registry.readDebugger();
    ASSERT_TRUE(readBack.has_value()) << readBack.error();
    EXPECT_EQ(Replacement::classify(*readBack, self).state, Replacement::State::ThisCopy);
    const auto marked = registry.readCreatedMarker();
    ASSERT_TRUE(marked.has_value()) << marked.error();
    EXPECT_EQ(*marked, !restorer.keyExisted()) << "the marker says whether TaskSmack made the key";

    // Starting Task Manager now starts the stand-in, with taskmgr.exe's path first: what main.cpp reads as a
    // start in Task Manager's place (#2039). The stub writes the command line Windows gave it.
    const ScratchFolder records(std::filesystem::temp_directory_path() / (L"TaskSmackCiIfeoRecord." + uniqueSuffix()));
    const std::filesystem::path record = records.path() / L"commandline.txt";
    Child started;
    {
        const ScopedVariable recordVariable(L"TASKSMACK_CI_STUB_RECORD", record.wstring());
        started = start(systemTaskManager());
    }
    ASSERT_NE(started.process(), nullptr);
    EXPECT_TRUE(samePath(imageOf(started.process()), exe.wstring()))
        << "Windows started " << Core::pathToUtf8(std::filesystem::path(imageOf(started.process()))) << ", not TaskSmack";
    const std::string commandLine = waitForText(record, STARTUP_WAIT);
    ASSERT_FALSE(commandLine.empty()) << "the stand-in never wrote its command line";
    const std::vector<std::string> arguments = argumentsOf(WinString::utf8ToWide(commandLine));
    ASSERT_GE(arguments.size(), 2U) << commandLine;
    EXPECT_TRUE(samePath(WinString::utf8ToWide(arguments[1]), systemTaskManager().wstring())) << commandLine;
    EXPECT_TRUE(App::parseLaunchOptions(arguments).asTaskManager) << commandLine;
    started.end();

    const ProcessActionResult cleared = Replacement::applyClear(registry);
    EXPECT_TRUE(cleared.success) << cleared.errorMessage;
    EXPECT_FALSE(taskManagerDebugger().has_value());
    EXPECT_EQ(keyExists(HKEY_LOCAL_MACHINE, taskManagerKey()), restorer.keyExisted()) << "a key TaskSmack made is deleted, another's kept";
    EXPECT_EQ(other.read(), std::wstring(OtherProgramsKey::debugger())) << "another program's IFEO key changed";
}

TEST_F(CiRealSystemTaskManagerTest, AnotherProgramsDebuggerIsNeitherReplacedNorRemoved)
{
    const TaskManagerKeyRestorer restorer;
    const std::wstring processExplorer = L"\"C:\\Tools\\procexp64.exe\" /e";
    writeString(HKEY_LOCAL_MACHINE, taskManagerKey(), debuggerValue(), processExplorer);
    Platform::Windows::WindowsIfeoRegistry registry;

    const ProcessActionResult set = Replacement::applySet(registry, R"(C:\Program Files\TaskSmack\TaskSmack.exe)", false);
    EXPECT_FALSE(set.success);
    EXPECT_EQ(set.errorMessage, Replacement::otherProgramMessage(R"(C:\Tools\procexp64.exe)"));
    EXPECT_EQ(taskManagerDebugger(), processExplorer);

    const ProcessActionResult cleared = Replacement::applyClear(registry);
    EXPECT_FALSE(cleared.success);
    EXPECT_EQ(taskManagerDebugger(), processExplorer);
    EXPECT_TRUE(keyExists(HKEY_LOCAL_MACHINE, taskManagerKey()));
}

// --- Start at sign-in: the real HKCU Run value (#3093) -------------------------------------------------------

class CiRealSystemStartAtLoginTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemStartAtLoginTest, TheRunValueIsWrittenReadAndRemoved)
{
    // Starting TaskSmack.exe --minimized from the value, and its window's state, need the app's own binary, so
    // they stay a manual check for now (#3490).
    if (const auto existing = readString(HKEY_CURRENT_USER, Platform::Windows::RUN_KEY, Platform::Windows::RUN_VALUE_NAME))
    {
        GTEST_SKIP() << "this user's Run key already has a TaskSmack value (" << WinString::wideToUtf8(*existing)
                     << "), which this test won't overwrite";
    }
    const ValueRemover removed(HKEY_CURRENT_USER, Platform::Windows::RUN_KEY, Platform::Windows::RUN_VALUE_NAME);
    Platform::WindowsStartAtLogin startAtLogin;

    const auto before = startAtLogin.read();
    ASSERT_TRUE(before.has_value()) << before.error();
    EXPECT_FALSE(before->command.has_value());
    EXPECT_TRUE(before->caseInsensitivePaths);
    EXPECT_TRUE(samePath(WinString::utf8ToWide(before->executable), ownExecutable().wstring())) << before->executable;

    const std::string command = Domain::StartAtLogin::runCommand(before->executable);
    const auto written = startAtLogin.write(command);
    ASSERT_TRUE(written.has_value()) << written.error();
    EXPECT_EQ(readString(HKEY_CURRENT_USER, Platform::Windows::RUN_KEY, Platform::Windows::RUN_VALUE_NAME), WinString::utf8ToWide(command));
    const auto after = startAtLogin.read();
    ASSERT_TRUE(after.has_value()) << after.error();
    EXPECT_EQ(after->command, command);

    // The Startup tab lists the same value, as Windows' own startup list does.
    Platform::WindowsStartupProbe probe;
    bool listed = false;
    for (const Platform::StartupEntry& entry : probe.enumerate())
    {
        if (entry.location == Platform::StartupLocation::RunUser && entry.registrationName == "TaskSmack")
        {
            listed = true;
            EXPECT_EQ(entry.command, command);
            EXPECT_TRUE(entry.enabled);
        }
    }
    EXPECT_TRUE(listed) << "the Startup tab doesn't list the TaskSmack Run value";

    const auto removedNow = startAtLogin.remove();
    ASSERT_TRUE(removedNow.has_value()) << removedNow.error();
    EXPECT_FALSE(readString(HKEY_CURRENT_USER, Platform::Windows::RUN_KEY, Platform::Windows::RUN_VALUE_NAME).has_value());
    const auto gone = startAtLogin.read();
    ASSERT_TRUE(gone.has_value()) << gone.error();
    EXPECT_FALSE(gone->command.has_value());
}

// --- The Startup tab's enable and disable: the real StartupApproved values (#801) ----------------------------

class CiRealSystemStartupTest : public TestSupport::CiRealSystemTest
{};

/// The entry named @p name at @p location, as the probe lists it now; nullopt when it doesn't.
[[nodiscard]] std::optional<Platform::StartupEntry> listedEntry(const std::string& name, Platform::StartupLocation location)
{
    Platform::WindowsStartupProbe probe;
    for (Platform::StartupEntry& entry : probe.enumerate())
    {
        if (entry.location == location && entry.registrationName == name)
        {
            return entry;
        }
    }
    return std::nullopt;
}

/// A scratch Run value under @p root, for TaskSmackCiStub.exe (which no one signs in to start), disabled and
/// enabled again as the Startup tab does: through the StartupApproved value, never the Run value itself.
void expectDisabledAndEnabled(HKEY root, Platform::StartupLocation location)
{
    const std::wstring name = L"TaskSmackCiStartup" + uniqueSuffix();
    const ValueRemover run(root, Platform::Windows::RUN_KEY, name);
    const ValueRemover approved(root, Platform::Windows::StartupMath::APPROVED_RUN_KEY, name);
    const std::wstring command = L"\"" + std::filesystem::path(TASKSMACK_CI_STUB_PATH).wstring() + L"\" 1";
    writeString(root, Platform::Windows::RUN_KEY, name, command);
    const std::string utf8Name = WinString::wideToUtf8(name);

    // Each read is checked before it is used: value_or() gives a default that fails the expectations after it.
    using Platform::Windows::StartupMath::ApprovedState;
    using Platform::Windows::StartupMath::parseStartupApproved;
    const std::optional<Platform::StartupEntry> fresh = listedEntry(utf8Name, location);
    ASSERT_TRUE(fresh.has_value()) << "the probe doesn't list the scratch Run value";
    const Platform::StartupEntry listed = fresh.value_or(Platform::StartupEntry{});
    EXPECT_TRUE(listed.enabled);
    EXPECT_EQ(listed.target, Platform::StartupTargetState::Present);

    Platform::WindowsStartupActions actions(true);
    const Platform::StartupActionResult disabled = actions.setEnabled(listed, false);
    ASSERT_TRUE(disabled.ok) << disabled.message;
    const std::optional<ApprovedState> disabledState =
        parseStartupApproved(readBinary(root, Platform::Windows::StartupMath::APPROVED_RUN_KEY, name));
    ASSERT_TRUE(disabledState.has_value()) << "no StartupApproved value was written";
    const ApprovedState disabledRecord = disabledState.value_or(ApprovedState{});
    EXPECT_FALSE(disabledRecord.enabled);
    EXPECT_NE(disabledRecord.disabledAtUnixSeconds, 0U);
    EXPECT_EQ(readString(root, Platform::Windows::RUN_KEY, name.c_str()), command) << "the Run value itself changed";
    const std::optional<Platform::StartupEntry> off = listedEntry(utf8Name, location);
    ASSERT_TRUE(off.has_value());
    const Platform::StartupEntry offEntry = off.value_or(Platform::StartupEntry{});
    EXPECT_FALSE(offEntry.enabled);
    EXPECT_NE(offEntry.disabledAtUnixSeconds, 0U);

    const Platform::StartupActionResult enabled = actions.setEnabled(offEntry, true);
    ASSERT_TRUE(enabled.ok) << enabled.message;
    const std::optional<ApprovedState> enabledState =
        parseStartupApproved(readBinary(root, Platform::Windows::StartupMath::APPROVED_RUN_KEY, name));
    ASSERT_TRUE(enabledState.has_value());
    EXPECT_TRUE(enabledState.value_or(ApprovedState{.enabled = false}).enabled);
    const std::optional<Platform::StartupEntry> on = listedEntry(utf8Name, location);
    ASSERT_TRUE(on.has_value());
    EXPECT_TRUE(on.has_value() && on.value_or(Platform::StartupEntry{}).enabled);
    EXPECT_EQ(readString(root, Platform::Windows::RUN_KEY, name.c_str()), command);
}

TEST_F(CiRealSystemStartupTest, AUserRunEntryIsDisabledAndEnabled)
{
    expectDisabledAndEnabled(HKEY_CURRENT_USER, Platform::StartupLocation::RunUser);
}

TEST_F(CiRealSystemStartupTest, AnAllUsersRunEntryIsDisabledAndEnabled)
{
    expectDisabledAndEnabled(HKEY_LOCAL_MACHINE, Platform::StartupLocation::RunMachine);
}

} // namespace
