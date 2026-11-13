// A program that tries to use TaskSmackHelper.exe on its own (#2549; #2572 review, finding 1), for
// test_BrokerHelper.cpp. The test copies it, as TaskSmack.exe, beside a copy of the helper in a folder
// of the test's own. From there it does all TaskSmack does: it starts the helper through HelperLauncher,
// though with CreateProcessW in place of ShellExecuteExW("runas"), so nothing runs elevated and no UAC
// prompt is raised. It then connects a PipeBroker and asks the helper to end a process. It is the
// helper's parent, and it is the TaskSmack.exe beside the helper, so the only thing the helper can
// refuse it for is the folder, which its user can change.
//
//   TaskSmack.exe kill <pid> <start ticks>
//
// Exit status 0 when the helper did as asked, which it must not. Otherwise it is 1, and why is in
// HelperTestParent.log beside it, which the test reads.
//
// The CI real-system tests (#3489, test_CiRealSystemHelper.cpp) also run it from an install the helper does
// serve (a Program Files folder only administrators can change, the parent a standard user), to watch how a
// served session ends:
//
//   TaskSmack.exe session <hold|close|idle|environment> <log> <pid> <start ticks> [<user> <password>]
//
// It starts the helper (as <user>, through CreateProcessWithLogonW, when one is given: an over-the-shoulder
// elevation's other account), writes "launched <helper pid>" to <log>,
// waits until the test makes <log>.ack (it holds the helper by then), and connects. Then:
//   - hold:  asks the helper to end <pid>, writes "served", and waits;
//   - close: the same, then closes the pipe (PipeBroker::disconnect()), writes "closed", and waits;
//   - idle:  writes "connected", and waits, asking nothing;
//   - environment: asks the helper for <pid>'s environment (#3610), writes "environment matched" when the read
//     has ENVIRONMENT_MARKER with the value this process inherited too (the test set it for both), and waits.
// "Waits" is two minutes at most: the test ends it sooner, or watches the helper end its session meanwhile.
// A failure is written to <log> as "failed: <why>", exit status 1.

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/PipeBroker.h"

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

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace
{

using namespace Platform;
using namespace Platform::Windows;

/// How long the helper may take to start: a freshly copied image may be scanned first.
constexpr std::chrono::milliseconds STARTUP{60'000};

/// ShellExecuteExW("runas") as far as it can be without elevating: the same request, checked, and the
/// helper started with CreateProcessW as this user, the process handed back as ShellExecuteExW does.
BOOL WINAPI createProcessInstead(SHELLEXECUTEINFOW* info)
{
    if (info == nullptr || info->cbSize != sizeof(SHELLEXECUTEINFOW) || info->lpVerb == nullptr ||
        std::wstring_view(info->lpVerb) != L"runas" || (info->fMask & SEE_MASK_NOCLOSEPROCESS) == 0 || info->lpFile == nullptr ||
        info->lpParameters == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::wstring commandLine = std::format(L"\"{}\" {}", info->lpFile, info->lpParameters);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(info->lpFile, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) ==
        FALSE)
    {
        return FALSE;
    }
    CloseHandle(process.hThread);
    info->hProcess = process.hProcess;
    return TRUE;
}

/// Records why the run failed in HelperTestParent.log beside this executable, and returns 1.
int fail(const std::string& why)
{
    std::array<wchar_t, 32'768> own{};
    const DWORD length = GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));
    const std::wstring log = BrokerHelper::besidePath(std::wstring_view(own.data(), length), L"HelperTestParent.log");
    if (!log.empty())
    {
        std::ofstream(log, std::ios::trunc) << why << '\n';
    }
    return 1;
}

template<typename T> [[nodiscard]] bool parseNumber(std::wstring_view text, T& value)
{
    std::string narrow;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9')
        {
            return false;
        }
        narrow.push_back(static_cast<char>(c));
    }
    const auto [end, error] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
    return error == std::errc{} && end == narrow.data() + narrow.size() && !narrow.empty();
}

/// The account the session verb starts the helper as, when it names one, and the helper it started. Globals:
/// the launcher seam is a plain function pointer.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) - read by the plain-function launcher seam
std::wstring g_LogonUser;
std::wstring g_LogonPassword;
std::wstring g_SessionLog;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Appends @p line to the session verb's log.
void note(std::string_view line)
{
    std::ofstream(g_SessionLog, std::ios::app) << line << '\n';
}

/// createProcessInstead(), as another account (g_LogonUser): CreateProcessWithLogonW, which needs no
/// privilege, makes the helper this process's child as "runas" does, and runs it as that account, as an
/// over-the-shoulder elevation does (#2550). No profile is loaded: the helper reads nothing of its own.
BOOL WINAPI createProcessAsAnotherAccount(SHELLEXECUTEINFOW* info)
{
    if (info == nullptr || info->cbSize != sizeof(SHELLEXECUTEINFOW) || info->lpVerb == nullptr ||
        std::wstring_view(info->lpVerb) != L"runas" || (info->fMask & SEE_MASK_NOCLOSEPROCESS) == 0 || info->lpFile == nullptr ||
        info->lpParameters == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::wstring commandLine = std::format(L"\"{}\" {}", info->lpFile, info->lpParameters);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessWithLogonW(g_LogonUser.c_str(),
                                L".",
                                g_LogonPassword.c_str(),
                                0,
                                info->lpFile,
                                commandLine.data(),
                                0,
                                nullptr,
                                nullptr,
                                &startup,
                                &process) == FALSE)
    {
        return FALSE;
    }
    CloseHandle(process.hThread);
    info->hProcess = process.hProcess;
    return TRUE;
}

/// How often the session verb looks for the test's acknowledgement.
constexpr DWORD ACK_POLL_MS = 10;

/// The launcher seam for the session verb: createProcessInstead() or createProcessAsAnotherAccount(), then
/// "launched <pid>" in the log, and a wait for the test's acknowledgement (<log>.ack) that it holds the helper's
/// process, so however quickly the session ends, the test sees how.
BOOL WINAPI launchAndNote(SHELLEXECUTEINFOW* info)
{
    const BOOL started = g_LogonUser.empty() ? createProcessInstead(info) : createProcessAsAnotherAccount(info);
    if (started == FALSE)
    {
        const DWORD error = GetLastError();
        note(std::format("start failed: {}", error));
        SetLastError(error);
        return FALSE;
    }
    note(std::format("launched {}", GetProcessId(info->hProcess)));
    // Holding the helper's process, so its PID names it still, until the test has opened it too: <log>.ack.
    const std::wstring acknowledgement = g_SessionLog + L".ack";
    const auto deadline = std::chrono::steady_clock::now() + STARTUP;
    while (GetFileAttributesW(acknowledgement.c_str()) == INVALID_FILE_ATTRIBUTES && std::chrono::steady_clock::now() < deadline)
    {
        Sleep(ACK_POLL_MS);
    }
    return TRUE;
}

/// The variable the environment mode looks for in the helper's read (test_CiRealSystemHelper.cpp sets it).
constexpr const char* ENVIRONMENT_MARKER = "TASKSMACK_CI_HELPER_ENVIRONMENT_3610";

/// What the environment mode found: "matched", or why not. Values are never written, only compared.
[[nodiscard]] std::string environmentOutcome(const std::expected<Broker::Response, std::string>& response)
{
    if (!response.has_value())
    {
        return "the call failed: " + response.error();
    }
    if (const auto* error = std::get_if<Broker::ErrorReply>(&*response))
    {
        return "the helper refused: " + error->message;
    }
    const auto* reply = std::get_if<Broker::EnvironmentReply>(&*response);
    if (reply == nullptr)
    {
        return "the helper answered with another reply";
    }
    if (reply->result.status != EnvironmentReadStatus::Ok)
    {
        return std::format("the read's status was {}: {}", static_cast<int>(reply->result.status), reply->result.detail);
    }
    std::array<char, 256> own{};
    const DWORD length = GetEnvironmentVariableA(ENVIRONMENT_MARKER, own.data(), static_cast<DWORD>(own.size()));
    if (length == 0 || length >= own.size())
    {
        return "this process has no marker to compare";
    }
    const std::string_view expected(own.data(), length);
    for (const EnvironmentVariable& variable : reply->result.variables)
    {
        if (variable.name == ENVIRONMENT_MARKER)
        {
            return variable.value == expected ? "matched" : "the marker's value differs";
        }
    }
    return std::format("no marker among {} variables", reply->result.variables.size());
}

/// Writes "failed: <why>" to the session verb's log, and returns 1.
int sessionFailed(const std::string& why)
{
    note("failed: " + why);
    return 1;
}

/// How long the session verb waits once it has done its part: the test ends it sooner.
constexpr DWORD SESSION_WAIT_MS = 120'000;

int session(std::span<wchar_t*> arguments)
{
    // session <mode> <log> <pid> <ticks> [<user> <password>]
    if (arguments.size() != 6 && arguments.size() != 8)
    {
        return fail("usage: session <hold|close|idle|environment> <log> <pid> <start ticks> [<user> <password>]");
    }
    const std::wstring_view mode(arguments[2]);
    g_SessionLog = arguments[3];
    if (arguments.size() == 8)
    {
        g_LogonUser = arguments[6];
        g_LogonPassword = arguments[7];
    }
    if (mode != L"hold" && mode != L"close" && mode != L"idle" && mode != L"environment")
    {
        return sessionFailed("unknown mode");
    }
    ProcessTarget target;
    if (!parseNumber(std::wstring_view(arguments[4]), target.pid) || !parseNumber(std::wstring_view(arguments[5]), target.startTimeTicks))
    {
        return sessionFailed("the target isn't <pid> <start ticks>");
    }
    PipeBroker broker(HelperLauncher::makeLauncher({.shellExecuteEx = &launchAndNote}));
    if (const auto connected = broker.connectWithin({}, STARTUP); !connected.has_value())
    {
        return sessionFailed("connect: " + connected.error());
    }
    if (mode == L"idle")
    {
        note("connected");
    }
    else if (mode == L"environment")
    {
        const std::string outcome = environmentOutcome(broker.call(Broker::EnvironmentRequest{.target = target}, {}));
        if (outcome != "matched")
        {
            return sessionFailed("environment: " + outcome);
        }
        note("environment matched");
    }
    else
    {
        const ProcessActionResult result =
            actionResultOf(broker.call(Broker::SignalRequest{.target = target, .signal = Broker::Signal::Kill}, {}));
        if (!result.success)
        {
            return sessionFailed("kill: " + result.errorMessage);
        }
        if (mode == L"close")
        {
            broker.disconnect();
            note("closed");
        }
        else
        {
            note("served");
        }
    }
    Sleep(SESSION_WAIT_MS);
    return 0;
}

int killThroughTheHelper(std::wstring_view pidText, std::wstring_view ticksText)
{
    ProcessTarget target;
    if (!parseNumber(pidText, target.pid) || !parseNumber(ticksText, target.startTimeTicks))
    {
        return fail("usage: kill <pid> <start ticks>");
    }
    PipeBroker broker(HelperLauncher::makeLauncher({.shellExecuteEx = &createProcessInstead}));
    if (const auto connected = broker.connectWithin({}, STARTUP); !connected.has_value())
    {
        return fail("connect: " + connected.error());
    }
    const ProcessActionResult result =
        actionResultOf(broker.call(Broker::SignalRequest{.target = target, .signal = Broker::Signal::Kill}, {}));
    if (!result.success)
    {
        return fail("kill: " + result.errorMessage);
    }
    return 0;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage) - the program's entry point
int wmain(int argc, wchar_t** argv)
{
    try
    {
        const std::span<wchar_t*> arguments(argv, static_cast<std::size_t>(argc));
        if (arguments.size() == 4 && std::wstring_view(arguments[1]) == L"kill")
        {
            return killThroughTheHelper(arguments[2], arguments[3]);
        }
        if (arguments.size() >= 2 && std::wstring_view(arguments[1]) == L"session")
        {
            return session(arguments);
        }
        return fail("usage: kill <pid> <start ticks>");
    }
    catch (const std::exception& error)
    {
        return fail(std::format("threw: {}", error.what()));
    }
}
