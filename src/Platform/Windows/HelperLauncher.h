#pragma once

// TaskSmack's PipeBroker::Launcher (#2549; #2517 step c): starts TaskSmackHelper.exe elevated with
// ShellExecuteExW("runas"), so Windows asks for consent, or an administrator's password, first.
//   - The helper is the TaskSmackHelper.exe beside TaskSmack.exe, by the full path GetModuleFileNameW
//     gives TaskSmack's own image (BrokerHelper::ownImage()): never looked for on a search path or in
//     the current directory.
//   - Its parameters are BrokerHelper::commandLine() for the pipe, TaskSmack's PID and TaskSmack's own
//     creation time, read by BrokerHelper::startTicksOf() as the helper reads its own (#2772). They are
//     checked to read back exactly, as the helper will split and parse them, before anything is started.
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
// again until a new helper starts, whose prompt may be answered with the user's own account (#2993).
// Under Administrator Protection (administratorProtection()) every elevation is another account, so
// there the reply holds while this TaskSmack runs.

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
#include <cstdint>
#include <expected>
#include <limits>
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

    ShellExecuteExFn shellExecuteEx = &shellExecuteWithCom;
    BrokerHelper::GetModuleFileNameFn getModuleFileName = &::GetModuleFileNameW;
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

/// UAC's TypeOfAdminApprovalMode policy value for Admin Approval Mode with Windows 11's Administrator
/// Protection (1 is the legacy mode; the value is absent where it was never set).
inline constexpr DWORD ADMINISTRATOR_PROTECTION_MODE = 2;

/// Whether Windows 11's Administrator Protection is on (#2993): UAC's TypeOfAdminApprovalMode policy,
/// under HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System, is
/// ADMINISTRATOR_PROTECTION_MODE. Every elevation then runs as a separate, system-managed administrator
/// account, never as the user. False when the value is absent or can't be read. One registry read: the
/// factory asks it once, at construction.
[[nodiscard]] bool administratorProtection() noexcept;

/// The words a read refused by a helper that runs as another account carries (#2769), in place of the
/// helper's own (BrokerHelper::OTHER_USERS_READ_REFUSED): why it was refused, and that "Read as
/// administrator" isn't offered again until a new helper starts (#2993).
inline constexpr std::string_view OTHER_USERS_READS_UNAVAILABLE =
    "The administrator account that approved the prompt isn't yours, so the administrator helper can't read other users' "
    "processes for you; \"Read as administrator\" isn't offered again until another prompt, such as Retry as "
    "administrator's, starts a new helper";

/// OTHER_USERS_READS_UNAVAILABLE's place under Administrator Protection (#2993), where every prompt is
/// answered with another account: "Read as administrator" won't be offered again while TaskSmack runs.
inline constexpr std::string_view ADMINISTRATOR_PROTECTION_READS_UNAVAILABLE =
    "Administrator Protection runs the administrator helper as a separate administrator account, never as yours, so it "
    "can't read other users' processes for you; \"Read as administrator\" won't be offered again until TaskSmack is "
    "restarted";

/// How long a LearnedReadsBroker keeps a helper's refusal (#2993).
enum class RefusalLasts : std::uint8_t
{
    ForItsHelper,  ///< Until a new helper session starts: each UAC prompt may be answered with another account.
    ForTheProcess, ///< While TaskSmack runs: Administrator Protection, where every elevation is another account.
};

/// TaskSmack's PipeBroker, with readsOtherUsers() learned from the helper's own answers (#2769). It
/// starts as the inner broker's, told from the token; a reply that says the helper reads only
/// TaskSmack's user's own processes (BrokerHelper::OTHER_USERS_READ_REFUSED, from a process read or a
/// system-wide one) makes it false, and that reply, which the user sees once, carries
/// OTHER_USERS_READS_UNAVAILABLE (ADMINISTRATOR_PROTECTION_READS_UNAVAILABLE for ForTheProcess)
/// instead. How long it stays false is @p lasts (#2993):
///   - RefusalLasts::ForItsHelper: the lesson belongs to the helper session that refused, as the inner
///     broker's sessionsStarted() counts them. It holds while that helper runs, and after it exits
///     until connect() starts another, whose UAC prompt may have been answered with the user's own
///     account (a credentials prompt is answered afresh each time): from then on it is the token's
///     again, until that helper refuses too. A transport that doesn't count its sessions keeps it for
///     good.
///   - RefusalLasts::ForTheProcess: for the rest of this process's life, since every elevation runs as
///     another account and no later prompt changes that.
/// Everything else is the inner broker's as it is. Thread-safe as the inner broker is: the lesson is one
/// atomic, the session that refused, stored (release) by whichever worker made the call and loaded
/// (acquire) by the UI thread before it loads the inner broker's count, so the count it compares is
/// never older than the one the lesson was taken at.
class LearnedReadsBroker final : public IPrivilegedBroker
{
  public:
    /// @throws std::invalid_argument when @p inner is null.
    explicit LearnedReadsBroker(std::unique_ptr<IPrivilegedBroker> inner, RefusalLasts lasts = RefusalLasts::ForItsHelper);

    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] bool lastSeenConnected() const noexcept override;
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& stopToken) override;
    [[nodiscard]] bool readsOtherUsers() const noexcept override;
    [[nodiscard]] std::uint64_t sessionsStarted() const noexcept override;
    [[nodiscard]] std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                    const std::stop_token& stopToken) override;
    [[nodiscard]] std::expected<Broker::Response, std::string> callIfIdle(const Broker::Request& request,
                                                                          std::chrono::milliseconds timeout) override;

  private:
    /// Takes the lesson from @p response, if it is the helper's refusal, and rewords it. The refusal is
    /// taken as the session's that sessionsStarted() counts after the call: the one that answered, or,
    /// when that one ended and another began as the reply came back, the newer one, which then doesn't
    /// offer the prompt either until a third starts. Never an older session's.
    void learnFrom(std::expected<Broker::Response, std::string>& response);

    /// m_RefusedIn before any refusal.
    static constexpr std::uint64_t NOT_REFUSED = std::numeric_limits<std::uint64_t>::max();

    std::unique_ptr<IPrivilegedBroker> m_Inner;
    bool m_ExpectedReadsOtherUsers = false; // the inner broker's readsOtherUsers() at construction, told from the token
    RefusalLasts m_Lasts = RefusalLasts::ForItsHelper;
    std::atomic<std::uint64_t> m_RefusedIn{NOT_REFUSED}; // the inner broker's sessionsStarted() at the latest refusal
};

} // namespace Platform::Windows::HelperLauncher

#endif // _WIN32
