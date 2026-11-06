// CI real-system tests of TaskSmackHelper.exe's session (#3489, for #3481 and the manual items 7, 9, 13 and 14 of
// #2782; tracker #3492). test_BrokerHelper.cpp runs the session in-process and the exe only as far as its
// refusals, since a served session needs an install its user can't change. A disposable runner has one: the
// test, an administrator there, makes a folder in Program Files that only administrators can change, puts the
// helper and TaskSmackHelperTestParent.exe (as TaskSmack.exe) in it, and runs that TaskSmack as a standard user
// (the runner's own account with Administrators deny-only). The helper then serves it for real, and each test
// watches one way a session ends: TaskSmack killed, TaskSmack closing the pipe, the idle timeout (the short-idle
// copy of the helper, 3 s), and an over-the-shoulder elevation's one action (a throwaway local account,
// deleted after). Each also checks the helper's exit status is Exit::Done, never std::terminate's abort (3) or a
// fail-fast. And a copy in a folder its user can change is refused, with the same standard user's token.
// CiRealSystem.h's guard skips all of it anywhere but such a runner.

#include "Platform/IProcessActions.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/WindowsHandles.h"
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
#include <aclapi.h>
#include <lm.h>
// clang-format on

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#pragma comment(lib, "netapi32.lib")

#if defined(TASKSMACK_WINDOWS_HELPER_PATH) && defined(TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH) &&                                        \
    defined(TASKSMACK_WINDOWS_HELPER_SHORT_IDLE_PATH)

namespace
{

using namespace std::chrono_literals;
using namespace TestSupport::CiRealSystem;
using Platform::ProcessTarget;
using Platform::Windows::UniqueHandle;
namespace BrokerHelper = Platform::Windows::BrokerHelper;

/// How soon the helper must exit once its session has ended (#2782 items 9 and 13).
constexpr std::chrono::milliseconds EXIT_WITHIN{2'000};
/// The short-idle helper's idle timeout (TASKSMACK_HELPER_TEST_IDLE_TIMEOUT_MS in tests/CMakeLists.txt).
constexpr std::chrono::milliseconds SHORT_IDLE{3'000};

/// The exit statuses an abnormal end gives: std::terminate's abort(), and a fail-fast (__fastfail, a /GS check).
constexpr DWORD ABORT_EXIT = 3;
constexpr DWORD FAST_FAIL_EXIT = 0xC0000409;

/// Fails unless @p code is Exit::Done: a session that ended as it should, not abort() (which std::terminate
/// calls, and which exits 3, Exit::NoParent's number too) or a fail-fast.
void expectSessionDone(std::optional<DWORD> code)
{
    ASSERT_TRUE(code.has_value()) << "the helper is still running";
    const DWORD exit = code.value_or(STILL_ACTIVE);
    EXPECT_NE(exit, FAST_FAIL_EXIT) << "the helper failed fast";
    EXPECT_NE(exit, ABORT_EXIT) << "the helper aborted (std::terminate), or refused its parent";
    EXPECT_EQ(exit, static_cast<DWORD>(BrokerHelper::Exit::Done)) << BrokerHelper::exitMeaning(exit);
}

/// An install the helper serves: a folder in Program Files only administrators can change, with @p helper as
/// TaskSmackHelper.exe and the test parent as TaskSmack.exe beside it. Removed on scope exit.
class ProtectedInstall
{
  public:
    explicit ProtectedInstall(const std::filesystem::path& helper) : m_Folder(programFiles() / (L"TaskSmackCiRealSystem." + uniqueSuffix()))
    {
        protectAsAnInstall(m_Folder.path());
        copyFile(helper, this->helper());
        copyFile(TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH, taskSmack());
        protectAsAnInstall(this->helper());
        protectAsAnInstall(taskSmack());
    }
    [[nodiscard]] std::filesystem::path helper() const
    {
        return m_Folder.path() / L"TaskSmackHelper.exe";
    }
    [[nodiscard]] std::filesystem::path taskSmack() const
    {
        return m_Folder.path() / L"TaskSmack.exe";
    }

  private:
    ScratchFolder m_Folder;
};

/// The session verb's log, in a folder of the test's own that the standard-user TaskSmack can write.
class SessionLog
{
  public:
    SessionLog() : m_Folder(std::filesystem::temp_directory_path() / (L"TaskSmackCiHelperLog." + uniqueSuffix()))
    {}
    [[nodiscard]] std::filesystem::path path() const
    {
        return m_Folder.path() / L"session.log";
    }
    /// The file the test makes once it holds the helper's process, which TaskSmack waits for.
    [[nodiscard]] std::filesystem::path acknowledgement() const
    {
        return m_Folder.path() / L"session.log.ack";
    }
    [[nodiscard]] std::string text() const
    {
        std::ifstream in(path());
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    /// Waits up to @p timeout for a line that is @p line, or starts with it and a space; its rest, or nullopt.
    /// A "failed:" line ends the wait at once.
    [[nodiscard]] std::optional<std::string> waitFor(std::string_view line, std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (bool first = true; first || std::chrono::steady_clock::now() < deadline; first = false)
        {
            std::istringstream lines(text());
            for (std::string each; std::getline(lines, each);)
            {
                if (each == line)
                {
                    return std::string{};
                }
                if (each.starts_with(std::string(line) + " "))
                {
                    return each.substr(line.size() + 1);
                }
                if (each.starts_with("failed:") || each.starts_with("start failed:"))
                {
                    return std::nullopt;
                }
            }
            std::this_thread::sleep_for(20ms);
        }
        return std::nullopt;
    }

  private:
    ScratchFolder m_Folder;
};

/// A throwaway local account, for an over-the-shoulder elevation: the helper runs as it, TaskSmack as the runner's
/// own user, so the helper gives the session one action (#2550). A plain user: on the runner UAC is on, so an
/// administrator's logon through CreateProcessWithLogonW would get the filtered token anyway, and the grant turns
/// only on the two accounts being different. Deleted on scope exit.
class ThrowawayAccount
{
  public:
    ThrowawayAccount()
    {
        std::random_device random;
        m_Name = std::format(L"tsci{:08x}", random());
        // Upper, lower, digits and symbols, 24 characters: any complexity policy takes it.
        m_Password = std::format(L"Ts!{:08x}{:08x}-qZ9", random(), random());
        USER_INFO_1 user{};
        user.usri1_name = m_Name.data();
        user.usri1_password = m_Password.data();
        user.usri1_priv = USER_PRIV_USER;
        user.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        DWORD badField = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - NetUserAdd
        // takes its USER_INFO_1 as bytes
        auto* userBytes = reinterpret_cast<LPBYTE>(&user);
        const NET_API_STATUS added = NetUserAdd(nullptr, 1, userBytes, &badField);
        if (added != NERR_Success)
        {
            ADD_FAILURE() << "couldn't add a local account: " << added << " (field " << badField << ")";
            return;
        }
        m_Added = true;
        m_Sid.resize(SECURITY_MAX_SID_SIZE);
        auto sidSize = static_cast<DWORD>(m_Sid.size());
        std::array<wchar_t, 256> domain{};
        auto domainSize = static_cast<DWORD>(domain.size());
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountNameW(nullptr, m_Name.c_str(), m_Sid.data(), &sidSize, domain.data(), &domainSize, &use) == FALSE)
        {
            ADD_FAILURE() << "couldn't read the new account's SID: " << GetLastError();
            m_Sid.clear();
            return;
        }
        m_Sid.resize(sidSize);
    }
    ~ThrowawayAccount()
    {
        if (m_Added)
        {
            NetUserDel(nullptr, m_Name.c_str());
        }
    }
    ThrowawayAccount(const ThrowawayAccount&) = delete;
    ThrowawayAccount& operator=(const ThrowawayAccount&) = delete;
    ThrowawayAccount(ThrowawayAccount&&) = delete;
    ThrowawayAccount& operator=(ThrowawayAccount&&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return !m_Sid.empty();
    }
    [[nodiscard]] const std::wstring& name() const noexcept
    {
        return m_Name;
    }
    [[nodiscard]] const std::wstring& password() const noexcept
    {
        return m_Password;
    }
    [[nodiscard]] const std::vector<std::byte>& sid() const noexcept
    {
        return m_Sid;
    }

  private:
    std::wstring m_Name;
    std::wstring m_Password;
    std::vector<std::byte> m_Sid;
    bool m_Added = false;
};

[[nodiscard]] std::optional<DWORD> parsePid(const std::string& text)
{
    DWORD pid = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (error != std::errc{} || end != text.data() + text.size() || pid == 0)
    {
        return std::nullopt;
    }
    return pid;
}

/// How long a test waits for the victim the helper ended: TerminateProcess returns before the process has gone.
constexpr std::chrono::milliseconds VICTIM_WAIT{10'000};

/// One served session: the standard-user TaskSmack started from @p install in @p mode, with an idle victim of its
/// user's for the helper to end. TaskSmack waits after starting the helper until the test holds the helper's
/// process (the session verb's acknowledgement file), so no way the session ends can beat the test to it. With
/// @p asAccount, the helper runs as that account, and TaskSmack's process, its token and the victim let it in, as an
/// elevated helper's administrator would be let in.
class ServedSession
{
  public:
    ServedSession(const ProtectedInstall& install, std::wstring_view mode, const ThrowawayAccount* asAccount = nullptr)
    {
        const std::vector<std::byte> other = (asAccount != nullptr) ? asAccount->sid() : std::vector<std::byte>{};
        m_Token = standardUserPrimaryToken(other);
        if (!m_Token)
        {
            return;
        }
        m_Victim = start(systemCmd(), {.token = m_Token.get(), .suspended = true, .parameters = L"/c exit 0"});
        const ProcessTarget target = m_Victim.target();
        std::wstring parameters = std::format(L"session {} \"{}\" {} {}", mode, m_Log.path().wstring(), target.pid, target.startTimeTicks);
        if (asAccount != nullptr)
        {
            parameters += std::format(L" {} {}", asAccount->name(), asAccount->password());
        }
        // Suspended until the other account is let in to TaskSmack's process and token, which the helper opens.
        m_TaskSmack = start(install.taskSmack(), {.token = m_Token.get(), .suspended = true, .parameters = parameters});
        if (asAccount != nullptr && m_TaskSmack.process() != nullptr)
        {
            letIn(m_Victim.process(), other, PROCESS_ALL_ACCESS);
            letIn(m_TaskSmack.process(), other, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION);
            HANDLE token = nullptr;
            if (OpenProcessToken(m_TaskSmack.process(), READ_CONTROL | WRITE_DAC, &token) != FALSE)
            {
                const UniqueHandle taskSmackToken(token);
                letIn(taskSmackToken.get(), other, TOKEN_QUERY | TOKEN_DUPLICATE);
            }
            else
            {
                ADD_FAILURE() << "couldn't open TaskSmack's token: " << GetLastError();
            }
        }
        m_TaskSmack.resume();
    }

    /// Waits for the helper's launch, opens it, and tells TaskSmack to go on; false, with the log in the failure,
    /// when there is no helper.
    [[nodiscard]] bool helperLaunched()
    {
        const std::optional<std::string> launched = m_Log.waitFor("launched", STARTUP_WAIT);
        const std::optional<DWORD> pid = launched.has_value() ? parsePid(*launched) : std::nullopt;
        if (!pid.has_value())
        {
            ADD_FAILURE() << "TaskSmack didn't start the helper. Its log:\n" << m_Log.text();
            return false;
        }
        // TaskSmack holds the helper's process until the acknowledgement, so its PID still names it.
        m_Helper.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, *pid));
        std::ofstream(m_Log.acknowledgement()) << "held\n";
        if (!m_Helper)
        {
            ADD_FAILURE() << "couldn't open the helper, PID " << *pid << ": " << GetLastError();
            return false;
        }
        return true;
    }
    /// Waits for TaskSmack to write @p line; false, with the log and the helper's exit status (if it has exited)
    /// in the failure, when it doesn't.
    [[nodiscard]] bool reached(std::string_view line)
    {
        if (!m_Log.waitFor(line, 2 * STARTUP_WAIT).has_value())
        {
            const std::optional<DWORD> exit = helperExit(0ms);
            ADD_FAILURE() << "TaskSmack never wrote \"" << line << "\". Its log:\n"
                          << m_Log.text() << "The helper: "
                          << (exit.has_value() ? BrokerHelper::exitMeaning(exit.value_or(0)) : std::string("still running"));
            return false;
        }
        return true;
    }
    /// The helper's exit status once it exits within @p timeout.
    [[nodiscard]] std::optional<DWORD> helperExit(std::chrono::milliseconds timeout) const
    {
        if (!m_Helper || WaitForSingleObject(m_Helper.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
        {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(m_Helper.get(), &code);
        return code;
    }
    [[nodiscard]] Child& taskSmack() noexcept
    {
        return m_TaskSmack;
    }
    /// Whether the helper ended the victim, waiting up to VICTIM_WAIT for it to be gone.
    [[nodiscard]] bool victimEnded() const
    {
        return m_Victim.exitCode(VICTIM_WAIT).has_value();
    }
    ~ServedSession()
    {
        m_TaskSmack.end();
        if (m_Helper && WaitForSingleObject(m_Helper.get(), 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(m_Helper.get(), 1);
            WaitForSingleObject(m_Helper.get(), static_cast<DWORD>(STARTUP_WAIT.count()));
        }
    }
    ServedSession(const ServedSession&) = delete;
    ServedSession& operator=(const ServedSession&) = delete;
    ServedSession(ServedSession&&) = delete;
    ServedSession& operator=(ServedSession&&) = delete;

  private:
    UniqueHandle m_Token;
    SessionLog m_Log;
    Child m_Victim;
    Child m_TaskSmack;
    UniqueHandle m_Helper;
};

class CiRealSystemHelperTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemHelperTest, ExitsWithinTwoSecondsOfTaskSmackBeingKilled)
{
    const ProtectedInstall install(TASKSMACK_WINDOWS_HELPER_PATH);
    ServedSession session(install, L"hold");
    ASSERT_TRUE(session.helperLaunched());
    ASSERT_TRUE(session.reached("served"));
    EXPECT_TRUE(session.victimEnded()) << "the helper served the action";
    ASSERT_EQ(session.helperExit(0ms), std::nullopt) << "the session ended before TaskSmack did";
    ASSERT_NE(TerminateProcess(session.taskSmack().process(), 1), FALSE) << GetLastError();
    expectSessionDone(session.helperExit(EXIT_WITHIN));
}

TEST_F(CiRealSystemHelperTest, ExitsWithinTwoSecondsOfTaskSmackClosingThePipe)
{
    const ProtectedInstall install(TASKSMACK_WINDOWS_HELPER_PATH);
    ServedSession session(install, L"close");
    ASSERT_TRUE(session.helperLaunched());
    ASSERT_TRUE(session.reached("closed"));
    // The helper's exit first, so the victim's wait doesn't eat into its two seconds.
    expectSessionDone(session.helperExit(EXIT_WITHIN));
    EXPECT_TRUE(session.victimEnded()) << "the helper served the action";
    EXPECT_EQ(session.taskSmack().exitCode(0ms), std::nullopt) << "TaskSmack still runs: the closed pipe ended it";
}

TEST_F(CiRealSystemHelperTest, ExitsAfterItsIdleTimeout)
{
    const ProtectedInstall install(TASKSMACK_WINDOWS_HELPER_SHORT_IDLE_PATH);
    ServedSession session(install, L"idle");
    ASSERT_TRUE(session.helperLaunched());
    ASSERT_TRUE(session.reached("connected"));
    const auto connected = std::chrono::steady_clock::now();
    // Not at once: the session waits out its idle timeout (it started a moment before "connected" was written).
    EXPECT_EQ(session.helperExit(SHORT_IDLE / 2), std::nullopt) << "the helper left well before its idle timeout";
    expectSessionDone(session.helperExit(SHORT_IDLE + EXIT_WITHIN));
    EXPECT_EQ(session.taskSmack().exitCode(0ms), std::nullopt) << "TaskSmack still runs, the pipe open: only the idle timeout ended it";
    RecordProperty(
        "idleExitMs",
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connected).count()));
}

TEST_F(CiRealSystemHelperTest, AnOverTheShoulderElevationExitsAfterItsOneAction)
{
    const ThrowawayAccount account;
    ASSERT_TRUE(account.ready());
    const ProtectedInstall install(TASKSMACK_WINDOWS_HELPER_PATH);
    // The helper runs as the other account, TaskSmack as the runner's user: a one-action grant (#2550).
    ServedSession session(install, L"hold", &account);
    ASSERT_TRUE(session.helperLaunched());
    ASSERT_TRUE(session.reached("served"));
    // The helper's exit first, so the victim's wait doesn't eat into its two seconds.
    expectSessionDone(session.helperExit(EXIT_WITHIN));
    EXPECT_TRUE(session.victimEnded()) << "the helper served the action";
    EXPECT_EQ(session.taskSmack().exitCode(0ms), std::nullopt) << "TaskSmack still runs, the pipe open: the one action ended it";
}

TEST_F(CiRealSystemHelperTest, ACopyInAFolderItsUserCanChangeIsRefused)
{
    // #2782 item 14, for real: the same standard user, the same two programs, but in a folder of %TEMP%, which
    // that user can change. The helper refuses its parent (Exit::NoParent) and the victim lives.
    const ScratchFolder folder(std::filesystem::temp_directory_path() / (L"TaskSmackCiUserCopy." + uniqueSuffix()));
    copyFile(TASKSMACK_WINDOWS_HELPER_PATH, folder.path() / L"TaskSmackHelper.exe");
    copyFile(TASKSMACK_WINDOWS_HELPER_TEST_PARENT_PATH, folder.path() / L"TaskSmack.exe");
    const UniqueHandle token = standardUserPrimaryToken();
    ASSERT_TRUE(token);
    const Child victim = start(systemCmd(), {.token = token.get(), .suspended = true, .parameters = L"/c exit 0"});
    const ProcessTarget target = victim.target();
    const Child taskSmack = start(folder.path() / L"TaskSmack.exe",
                                  {.token = token.get(), .parameters = std::format(L"kill {} {}", target.pid, target.startTimeTicks)});
    ASSERT_NE(taskSmack.process(), nullptr);
    EXPECT_EQ(taskSmack.exitCode(2 * STARTUP_WAIT), std::optional<DWORD>(1)) << "it got a broker";
    std::ifstream in(folder.path() / L"HelperTestParent.log");
    const std::string log{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_TRUE(log.contains(BrokerHelper::exitMeaning(static_cast<DWORD>(BrokerHelper::Exit::NoParent)))) << log;
    EXPECT_EQ(victim.exitCode(0ms), std::nullopt) << "the helper ended the victim";
}

} // namespace

#endif
