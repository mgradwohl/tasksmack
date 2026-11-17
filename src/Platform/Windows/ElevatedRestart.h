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
//     bounded by HANDOFF_PATIENCE) is for this process and no other that took its PID.
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
#include "Platform/IHandedOverFolder.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/WindowsHandles.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Platform::Windows::ElevatedRestart
{

/// The longest the elevated copy waits for the Standard one to close: the whole hand-off's HANDOFF_PATIENCE, whose
/// rest the instance lock then gets (handoffLockPatience()).
inline constexpr DWORD HANDOFF_WAIT_MS = static_cast<DWORD>(HANDOFF_PATIENCE.count());
/// How often that wait reports how long it has waited (#3781).
inline constexpr DWORD HANDOFF_PROGRESS_EVERY_MS = static_cast<DWORD>(HANDOFF_PROGRESS_EVERY.count());

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
/// may not open (another account's elevation) is Unopenable, which the instance lock then covers (main.cpp). It waits
/// in slices of @p progressEveryMs, telling @p onProgress, when set, how long it has waited after each one it is still
/// running at (#3781).
[[nodiscard]] HandoffWait waitForHandoff(DWORD pid,
                                         std::uint64_t startTicks,
                                         DWORD timeoutMs = HANDOFF_WAIT_MS,
                                         const WaitFunctions& functions = {},
                                         const HandoffProgress& onProgress = {},
                                         DWORD progressEveryMs = HANDOFF_PROGRESS_EVERY_MS);

/// The file's hard-link count, opened without following a link; nullopt when it can't be read.
[[nodiscard]] std::optional<DWORD> linkCountOf(const std::wstring& path);

/// Whether the owner of the file or folder @p handle (opened with READ_CONTROL) is an administrator or system account
/// (BUILTIN\Administrators, SYSTEM, TrustedInstaller); nullopt when it can't be read.
[[nodiscard]] std::optional<bool> ownerIsPrivileged(HANDLE handle);

/// The folder @p path opened to be held: without FILE_SHARE_DELETE, so it can't be renamed or deleted while open,
/// and without following a link at its last component. INVALID_HANDLE_VALUE when it can't be opened.
[[nodiscard]] HANDLE openToPin(const std::wstring& path);

/// The calls checkConfigDir() makes on the disk: the system's, or a test's fakes (#3787).
struct ConfigDirFunctions
{
    using AttributesFn = DWORD(WINAPI*)(LPCWSTR);
    using LinkCountFn = std::optional<DWORD> (*)(const std::wstring&);
    using OpenToPinFn = HANDLE (*)(const std::wstring&);
    using FileInformationFn = BOOL(WINAPI*)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
    using FinalPathFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
    using OwnerIsPrivilegedFn = std::optional<bool> (*)(HANDLE);

    AttributesFn getFileAttributes = &::GetFileAttributesW;
    LinkCountFn linkCount = &linkCountOf;
    OpenToPinFn openToPin = &ElevatedRestart::openToPin;
    FileInformationFn getFileInformation = &::GetFileInformationByHandle;        ///< Through the held handle
    FinalPathFn getFinalPath = &::GetFinalPathNameByHandleW;                     ///< Through the held handle
    OwnerIsPrivilegedFn ownerIsPrivileged = &ElevatedRestart::ownerIsPrivileged; ///< Through the held handle
};

/// Whether @p dir, a config directory an elevated restart was handed (--config-dir), may be used (#2728): an
/// absolute local path inside a user's profile folder under @p profilesRoot (the user profiles folder, C:\Users): two
/// folders below it or more (C:\Users\pat\TaskSmack, never C:\Users\pat itself),
/// with no "." or ".." in it; every folder from the drive down to it a real folder, never a junction, link or
/// other reparse point; config.toml and tasksmack.lock in it, where they exist, neither reparse points nor
/// hard-linked elsewhere. Then it is held open, and the rest is checked through that handle (#3775), so it is the
/// folder held that passes: still a folder with no reparse point, its final path @p dir itself, its owner no
/// administrator or system account. The handle is the caller's to hold (#3787); the error says which check failed.
[[nodiscard]] std::expected<UniqueHandle, std::string>
checkConfigDir(const std::filesystem::path& dir, const std::filesystem::path& profilesRoot, const ConfigDirFunctions& functions = {});

/// checkConfigDir() against this machine's user profiles folder (FOLDERID_UserProfiles).
[[nodiscard]] std::expected<UniqueHandle, std::string> checkConfigDir(const std::filesystem::path& dir);

/// Whether the owner of @p handle (opened with READ_CONTROL) is this process's token user or BUILTIN\Administrators,
/// and no one else (#4106): the owners an elevated session's own config folder may have. nullopt when it can't be read.
[[nodiscard]] std::optional<bool> ownerIsOwnAccountOrAdministrators(HANDLE handle);

/// This process token's own profile folder, from HKLM's ProfileList (GetUserProfileDirectoryW), not a user-writable
/// source; nullopt when it can't be read. The folder the own config folder must sit inside (#4106).
[[nodiscard]] std::optional<std::filesystem::path> currentUserProfileDirectory();

/// Whether @p ownerAcceptable returns true for a handle's owner: the real one is ownerIsOwnAccountOrAdministrators.
using OwnerAcceptableFn = std::optional<bool> (*)(HANDLE);

/// The account's own config folder @p dir (FOLDERID_RoamingAppData\TaskSmack) for an elevated session that took no
/// hand-off (#4106), held as a handed-over one is: its environment and its known folders are the less-privileged side's
/// (HKCU), which can redirect AppData, so the folder must be anchored to the token's own profile and its owner checked.
///   - @p profileDir is this token's own profile (currentUserProfileDirectory()); @p dir, and the parent when it is
///     created, must sit inside it, so a redirected AppData under another profile is refused.
///   - It is checked and held by checkConfigDir() against @p profilesRoot (the user profiles folder), but its owner is
///     required to pass @p ownerAcceptable (this account or Administrators), not refused for being an administrator's:
///     an elevated TaskSmack made its own folder owned by Administrators, and SYSTEM, TrustedInstaller or another user
///     is refused. The same rule holds for the parent when the folder is made in it.
///   - Not there yet (a first elevated run), it is made in its parent, which is checked, held and owner-checked the same
///     way, relative to that handle, so nothing is made behind a junction or in another account's folder.
/// The handle is the caller's to hold; the error says which check failed.
[[nodiscard]] std::expected<UniqueHandle, std::string>
checkOwnConfigDir(const std::filesystem::path& dir,
                  const std::filesystem::path& profileDir,
                  const std::filesystem::path& profilesRoot,
                  ConfigDirFunctions functions = {},
                  OwnerAcceptableFn ownerAcceptable = &ownerIsOwnAccountOrAdministrators);

/// The account's own config folder, FOLDERID_RoamingAppData\TaskSmack (UserConfig's, when nothing overrides it), checked
/// and held by checkOwnConfigDir() against this machine's user profiles folder (#4106). The folder's path, and the
/// folder held or why not.
struct OwnConfigDir
{
    std::filesystem::path path; ///< Empty when the known folder can't be read
    std::expected<std::shared_ptr<IHandedOverFolder>, std::string> held;
};
[[nodiscard]] OwnConfigDir holdOwnConfigDir();

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
