#pragma once

// Restart as administrator on Windows (#2728, IElevatedRestart.h): TaskSmack.exe started again by its own full
// path (GetModuleFileNameW, never a search path) with ShellExecuteExW's "runas" verb, so Windows raises the UAC
// prompt first, in front of TaskSmack's window. Its parameters are "--handoff <this PID> <its creation ticks> --config-dir
// "<the launching user's config directory>"", checked to read back exactly as the new copy's
// CommandLineToArgvW will split them before anything starts.
//   - A dismissed prompt (ERROR_CANCELLED) is Declined; anything else that stops it is Failed.
//   - Started only with the new process's handle back, so this copy closes only once Windows has started the
//     elevated one.
//   - The parameters carry this process's creation time too, so the elevated copy's wait (waitForHandoff(),
//     bounded by HANDOFF_WAIT_MS) is for this process and no other that took its PID.
//   - The elevated copy takes the handed-over directory only once checkConfigDir() passes (the trust boundary,
//     Platform/IElevatedRestart.h).
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
#include <optional>
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

/// The elevated copy's parameters for Standard TaskSmack @p pid, created at @p startTicks, and @p configDir, once
/// it is checked that CommandLineToArgvW splits them back into exactly those. The error says they wouldn't.
[[nodiscard]] std::expected<std::wstring, std::string>
parameters(DWORD pid, std::uint64_t startTicks, const std::filesystem::path& configDir);

/// Starts this executable elevated for @p configDir, on behalf of process @p pid created at @p startTicks (this
/// process and its own creation time, by default). Blocks while the prompt is up. @p owner, TaskSmack's main window
/// or null, owns the prompt.
[[nodiscard]] ElevatedRestartResult restart(const std::filesystem::path& configDir,
                                            HWND owner,
                                            const Functions& functions = {},
                                            DWORD pid = GetCurrentProcessId(),
                                            std::optional<std::uint64_t> startTicks = std::nullopt);

/// What the prompt will ask a token of @p elevationType for: approval for a split-token administrator
/// (TokenElevationTypeLimited), an administrator's name and password otherwise.
[[nodiscard]] AdministratorPrompt promptFor(TOKEN_ELEVATION_TYPE elevationType) noexcept;

/// The calls waitForHandoff() makes: the system's, or a test's fakes.
struct WaitFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using WaitForSingleObjectFn = DWORD(WINAPI*)(HANDLE, DWORD);

    OpenProcessFn openProcess = &::OpenProcess;
    WaitForSingleObjectFn waitForSingleObject = &::WaitForSingleObject;
};

/// Waits up to @p timeoutMs for process @p pid, the Standard TaskSmack that restarted this one, to exit, once its
/// creation time is @p startTicks. Only ERROR_INVALID_PARAMETER (no such PID) is NotRunning; a process this account
/// may not open (another account's elevation) is Unopenable, which the instance lock then covers (main.cpp).
[[nodiscard]] HandoffWait
waitForHandoff(DWORD pid, std::uint64_t startTicks, DWORD timeoutMs = HANDOFF_WAIT_MS, const WaitFunctions& functions = {});

/// The calls checkConfigDir() makes on the disk: the system's, or a test's fakes. Each takes a full path.
struct ConfigDirFunctions
{
    /// GetFileAttributesW.
    using AttributesFn = DWORD(WINAPI*)(LPCWSTR);
    /// The file's hard-link count, opened without following a link; nullopt when it can't be read.
    using LinkCountFn = std::optional<DWORD> (*)(const std::wstring&);
    /// Whether the folder's owner is an administrator or system account (BUILTIN\Administrators, SYSTEM,
    /// TrustedInstaller); nullopt when it can't be read.
    using OwnerIsPrivilegedFn = std::optional<bool> (*)(const std::wstring&);
    /// Holds the folder open for the rest of this process, without FILE_SHARE_DELETE so it can't be renamed or
    /// replaced, checking through the open handle that it is still no reparse point. False when it can't.
    using PinFn = bool (*)(const std::wstring&);

    AttributesFn getFileAttributes = &::GetFileAttributesW;
    LinkCountFn linkCount = nullptr;                 ///< Null: the real one
    OwnerIsPrivilegedFn ownerIsPrivileged = nullptr; ///< Null: the real one
    PinFn pin = nullptr;                             ///< Null: the real one
};

/// Whether @p dir, a config directory an elevated restart was handed (--config-dir), may be used (#2728): an
/// absolute local path under @p profilesRoot (the user profiles folder, C:\Users), at least one folder below it,
/// with no "." or ".." in it; every folder from the drive down to it a real folder, never a junction, link or
/// other reparse point; its owner no administrator or system account; config.toml and tasksmack.lock in it, where
/// they exist, neither reparse points nor hard-linked elsewhere; and held open (pinned) from then on. The error
/// says which check failed.
[[nodiscard]] std::expected<void, std::string>
checkConfigDir(const std::filesystem::path& dir, const std::filesystem::path& profilesRoot, const ConfigDirFunctions& functions = {});

/// checkConfigDir() against this machine's user profiles folder (FOLDERID_UserProfiles).
[[nodiscard]] std::expected<void, std::string> checkConfigDir(const std::filesystem::path& dir);
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
