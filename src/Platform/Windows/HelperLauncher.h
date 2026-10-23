#pragma once

// TaskSmack's PipeBroker::Launcher (#2549; #2517 step c): starts TaskSmackHelper.exe elevated with
// ShellExecuteExW("runas"), so Windows asks for consent, or an administrator's password, first.
//   - The helper is the TaskSmackHelper.exe beside TaskSmack.exe, by the full path GetModuleFileNameW
//     gives TaskSmack's own image: never looked for on a search path or in the current directory.
//   - Its parameters are BrokerHelper::commandLine() for the pipe, TaskSmack's PID and TaskSmack's own
//     creation time. They are checked to read back exactly, as the helper will split and parse them,
//     before anything is started.
//   - A dismissed prompt (ERROR_CANCELLED) is PROMPT_DISMISSED, which callers take as a refusal, as a
//     dismissed pkexec prompt is on Linux; a missing helper is HELPER_MISSING.
//   - ShellExecuteExW blocks while the prompt is up, with no time limit: PipeBroker calls this on a
//     thread of its own, outside its lock.
//   - The prompt is owned by TaskSmack's main window (#2550), so it comes up in front of TaskSmack,
//     not behind it or only in the taskbar.
//   - "runas" makes the helper TaskSmack's child: AppInfo creates it with TaskSmack as its parent
//     process, which the helper checks (BrokerHelper::checkParentIsTaskSmack()).
//
// The Win32 calls are a table of function pointers, so the tests drive every answer with fakes and never
// raise a UAC prompt (test_HelperLauncher.cpp).
//
// Whether the helper will read other users' processes, which is what "Read as administrator" is for, is
// first told from TaskSmack's token (readsOtherUsers()), and then learned from the helper itself
// (LearnedReadsBroker, #2769): a helper that answers a read with BrokerHelper::OTHER_USERS_READ_REFUSED
// runs as another account than TaskSmack's user, so from that reply on no reader offers the prompt
// again while this TaskSmack runs.

#ifdef _WIN32
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

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/PipeBroker.h"
#include "Platform/Windows/WindowsHandles.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace Platform::Windows::HelperLauncher
{

/// The helper's file name, beside TaskSmack.exe.
inline constexpr std::wstring_view HELPER_FILE_NAME = L"TaskSmackHelper.exe";
/// The error when the user dismisses the UAC prompt.
inline constexpr std::string_view PROMPT_DISMISSED = "the UAC prompt was dismissed";
/// The error when there is no TaskSmackHelper.exe beside TaskSmack.exe.
inline constexpr std::string_view HELPER_MISSING = "TaskSmackHelper.exe isn't in TaskSmack's folder";

/// ShellExecuteExW on a thread whose COM is initialised, as the shell asks of its callers: the default
/// Functions::shellExecuteEx. Leaves GetLastError() as ShellExecuteExW set it.
BOOL WINAPI shellExecuteWithCom(SHELLEXECUTEINFOW* info);

/// The calls launch() makes: the system's, or a test's fakes.
struct Functions
{
    using ShellExecuteExFn = BOOL(WINAPI*)(SHELLEXECUTEINFOW*);
    using GetModuleFileNameFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

    ShellExecuteExFn shellExecuteEx = &shellExecuteWithCom;
    GetModuleFileNameFn getModuleFileName = &::GetModuleFileNameW;
};

/// @p arguments as the helper's parameters: BrokerHelper::commandLine(), once it is checked that
/// CommandLineToArgvW splits it, and BrokerHelper::parseArguments() reads it, back into exactly
/// @p arguments. The error says it doesn't.
[[nodiscard]] std::expected<std::wstring, std::string> parameters(const BrokerHelper::Arguments& arguments);

/// What a failed ShellExecuteExW's @p error means for TaskSmack: PROMPT_DISMISSED, HELPER_MISSING, or
/// the system's words.
[[nodiscard]] std::string launchError(DWORD error);

/// Starts the helper beside this executable elevated, to serve @p pipeName for this process, and
/// returns its process. Blocks while the UAC prompt is up; @p stopToken is looked at only before the
/// prompt, which nothing can take down. @p owner, TaskSmack's main window or null, owns the prompt. The
/// error says why there is no helper.
[[nodiscard]] std::expected<UniqueHandle, std::string>
launch(const std::wstring& pipeName, const std::stop_token& stopToken, const Functions& functions = {}, HWND owner = nullptr);

/// launch() with @p functions and @p owner as a PipeBroker::Launcher: what TaskSmack's PipeBroker is
/// built with.
[[nodiscard]] PipeBroker::Launcher makeLauncher(Functions functions = {}, HWND owner = nullptr);

/// Why this TaskSmack can't use the helper, or nothing when it can (#2550), before anything offers
/// "Retry as administrator": TaskSmack already runs elevated, there is no TaskSmackHelper.exe beside
/// it, or its user can change the install (BrokerHelper::checkUnchangeableBy()), which the helper
/// would refuse. @p ownImage is TaskSmack's own executable; @p installCheck is
/// BrokerHelper::checkUnchangeableByParent() run against this process, or a test's.
[[nodiscard]] std::expected<void, std::string>
availability(bool elevated, const std::wstring& ownImage, BrokerHelper::Self::InstallCheck installCheck);

/// availability() for this process, as the factory asks it.
[[nodiscard]] std::expected<void, std::string> availability();

/// Whether a helper started from a token of @p elevationType is expected to run as that token's own
/// user, and so to read other users' processes (#2550): TokenElevationTypeLimited, a split-token
/// administrator, whose UAC prompt only asks for consent. TokenElevationTypeDefault, unelevated, is a
/// standard user (or UAC is off): the prompt asks for an administrator's password, the helper runs as
/// that administrator, and it then reads only the user's own processes (BrokerHelper::sessionPolicy()),
/// which the user can read already. Full is already elevated, and needs no helper.
/// With Windows 11's Administrator Protection, even a Limited administrator's elevation runs as a
/// separate system-managed account, and a credentials prompt can be answered with another
/// administrator's account: the helper then gives the over-the-shoulder session too. This can't tell
/// that beforehand; the helper's own check is the one that counts, and LearnedReadsBroker takes the
/// answer from its first refusal (#2769).
[[nodiscard]] bool readsOtherUsers(TOKEN_ELEVATION_TYPE elevationType) noexcept;

/// readsOtherUsers() for this process's token: false when it can't be read.
[[nodiscard]] bool readsOtherUsers() noexcept;

/// The words a read refused by a helper that runs as another account carries (#2769), in place of the
/// helper's own (BrokerHelper::OTHER_USERS_READ_REFUSED): why it was refused, and that "Read as
/// administrator" won't be offered again.
inline constexpr std::string_view OTHER_USERS_READS_UNAVAILABLE =
    "The administrator account that approved the prompt isn't yours, so the administrator helper can't read other users' "
    "processes for you; \"Read as administrator\" won't be offered again until TaskSmack is restarted";

/// TaskSmack's PipeBroker, with readsOtherUsers() learned from the helper's own answers (#2769). It
/// starts as the inner broker's, told from the token; a reply that says the helper reads only
/// TaskSmack's user's own processes (BrokerHelper::OTHER_USERS_READ_REFUSED, from a process read or a
/// system-wide one) makes it false for the rest of this process's life, and that reply, which the user
/// sees once, carries OTHER_USERS_READS_UNAVAILABLE instead. Nothing makes it true again: the helper's
/// account is the elevation's, which doesn't change while TaskSmack runs. Everything else is the inner
/// broker's as it is. Thread-safe as the inner broker is: the learned answer is one atomic, read by the
/// UI thread and set by whichever worker made the call.
class LearnedReadsBroker final : public IPrivilegedBroker
{
  public:
    /// @throws std::invalid_argument when @p inner is null.
    explicit LearnedReadsBroker(std::unique_ptr<IPrivilegedBroker> inner);

    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& stopToken) override;
    [[nodiscard]] bool readsOtherUsers() const noexcept override;
    [[nodiscard]] std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                    const std::stop_token& stopToken) override;
    [[nodiscard]] std::expected<Broker::Response, std::string> callIfIdle(const Broker::Request& request,
                                                                          std::chrono::milliseconds timeout) override;

  private:
    /// Takes the lesson from @p response, if it is the helper's refusal, and rewords it.
    void learnFrom(std::expected<Broker::Response, std::string>& response);

    std::unique_ptr<IPrivilegedBroker> m_Inner;
    std::atomic<bool> m_ReadsOtherUsers;
};

} // namespace Platform::Windows::HelperLauncher

#endif // _WIN32
