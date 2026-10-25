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

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
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
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

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
        return fail("usage: kill <pid> <start ticks>");
    }
    catch (const std::exception& error)
    {
        return fail(std::format("threw: {}", error.what()));
    }
}
