#pragma once

// Restart as administrator on Windows (#2728, IElevatedRestart.h): TaskSmack.exe started again by its own full
// path (GetModuleFileNameW, never a search path) with ShellExecuteExW's "runas" verb, so Windows raises the UAC
// prompt first, in front of TaskSmack's window. Its parameters are "--handoff <this PID> --config-dir
// "<the launching user's config directory>"", checked to read back exactly as the new copy's
// CommandLineToArgvW will split them before anything starts.
//   - A dismissed prompt (ERROR_CANCELLED) is Declined; anything else that stops it is Failed.
//   - Started only with the new process's handle back, so this copy closes only once Windows has started the
//     elevated one.
//   - The elevated copy waits for this one to exit (waitForHandoff(), bounded by HANDOFF_WAIT_MS) before it
//     reads the settings this one saves as it closes, or takes the single-instance lock.
//
// The Win32 calls are function pointers, so the tests drive every answer with fakes and never raise a UAC
// prompt (test_ElevatedRestart.cpp).

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

#include "Platform/IElevatedRestart.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/HelperLauncher.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace Platform::Windows::ElevatedRestart
{

/// The longest the elevated copy waits for the Standard one to close before it starts anyway: the Standard
/// one saves its settings and exits within a second or two; one that hangs must not stop Administrator.
inline constexpr DWORD HANDOFF_WAIT_MS = 15'000;

/// The calls restart() makes: the system's, or a test's fakes.
struct Functions
{
    using ShellExecuteExFn = BOOL(WINAPI*)(SHELLEXECUTEINFOW*);

    ShellExecuteExFn shellExecuteEx = &HelperLauncher::shellExecuteWithCom;
    BrokerHelper::GetModuleFileNameFn getModuleFileName = &::GetModuleFileNameW;
};

/// The elevated copy's parameters for Standard TaskSmack @p pid and @p configDir, once it is checked that
/// CommandLineToArgvW splits them back into exactly those. The error says they wouldn't.
[[nodiscard]] std::expected<std::wstring, std::string> parameters(DWORD pid, const std::filesystem::path& configDir);

/// Starts this executable elevated for @p configDir, on behalf of process @p pid (this one's, by default).
/// Blocks while the prompt is up. @p owner, TaskSmack's main window or null, owns the prompt.
[[nodiscard]] ElevatedRestartResult
restart(const std::filesystem::path& configDir, HWND owner, const Functions& functions = {}, DWORD pid = GetCurrentProcessId());

/// What the prompt will ask a token of @p elevationType for: approval for a split-token administrator
/// (TokenElevationTypeLimited), an administrator's name and password otherwise.
[[nodiscard]] AdministratorPrompt promptFor(TOKEN_ELEVATION_TYPE elevationType) noexcept;

/// How the wait for the Standard copy ended.
enum class HandoffWait : std::uint8_t
{
    Exited,     ///< It closed
    NotRunning, ///< It had already gone (or can't be opened)
    TimedOut,   ///< It still runs after the wait: start anyway
};

/// The calls waitForHandoff() makes: the system's, or a test's fakes.
struct WaitFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using WaitForSingleObjectFn = DWORD(WINAPI*)(HANDLE, DWORD);

    OpenProcessFn openProcess = &::OpenProcess;
    WaitForSingleObjectFn waitForSingleObject = &::WaitForSingleObject;
};

/// Waits up to @p timeoutMs for process @p pid, the Standard TaskSmack that restarted this one, to exit.
[[nodiscard]] HandoffWait waitForHandoff(DWORD pid, DWORD timeoutMs = HANDOFF_WAIT_MS, const WaitFunctions& functions = {});

/// The IElevatedRestart a Standard TaskSmack is given (Platform::makeElevatedRestart()).
class WindowsElevatedRestart final : public IElevatedRestart
{
  public:
    WindowsElevatedRestart(HWND owner, AdministratorPrompt prompt, Functions functions = {}) noexcept
        : m_Owner(owner), m_Prompt(prompt), m_Functions(functions)
    {}

    [[nodiscard]] AdministratorPrompt prompt() const noexcept override
    {
        return m_Prompt;
    }

    [[nodiscard]] ElevatedRestartResult restart(const std::filesystem::path& configDir) override
    {
        return ElevatedRestart::restart(configDir, m_Owner, m_Functions);
    }

  private:
    HWND m_Owner;
    AdministratorPrompt m_Prompt;
    Functions m_Functions;
};

} // namespace Platform::Windows::ElevatedRestart

#endif // _WIN32
