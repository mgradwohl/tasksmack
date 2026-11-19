#pragma once

// Windows IProcessModulesReader (#802): EnumProcessModulesEx(LIST_MODULES_ALL) on a handle opened
// with PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, each module's path and image size, and
// its file version from the file's VERSIONINFO, cached by the file's name. The calls it makes are a table of
// function pointers, the system's by default, so tests can feed it fixed answers and denials
// (test_WindowsProcessModules.cpp).
//
// The path shown is GetModuleFileNameExW's: the module's FullDllName in the process's own loader data, which the
// process can rewrite to anything, so it is shown and never opened (#4373). The file the version is read from is
// named by the kernel instead: GetMappedFileNameW at the module's base, the NT name of the file its image section
// was made from. Only a file on a local volume is opened, and its version is read through the handle
// (Windows::heldFileVersion()), never with GetFileVersionInfoW, which opens a file by name. A module mapped from a
// network share, or from a device TaskSmack can't tell is a local drive, keeps its row with versionNotRead set and no
// version: opening it could connect, with TaskSmack's or the elevated helper's credentials, to wherever it is.
//
// The open (readLocalFileVersion()) is Windows::openLocalNtFile() (WindowsLocalNtFile.h, #4482), the one the image
// file the Publisher column and Look up on VirusTotal read is opened with too: NtCreateFile on that NT name itself,
// with OBJ_DONT_REPARSE and FILE_OPEN_REPARSE_POINT, so a junction or symlink met on the way, which the process could
// have put there since it was named (one to \\host\share, say), fails the open rather than being followed to a host of
// the process's choosing; identification-level security only; and a handle that is a link, off a local disk or a
// redirector's, closed unread.
//
// What this can't rule out: a process can map another local file's image where its own module was, and its own
// files' version info says whatever their author wrote. A version is the file's own claim either way.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
// clang-format on

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "WindowsLocalNtFile.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace Platform::Windows
{

/// Reads the version of the file a handle is open on (Windows::heldFileVersion()).
using ReadHeldVersionFn = std::optional<ModuleVersion> (*)(HANDLE);

/// The version of the file whose NT name is @p ntName ("\Device\HarddiskVolume3\Windows\System32\ntdll.dll", as
/// GetMappedFileNameW names it), opened by Windows::openLocalNtFile() through @p fns (NtCreateFile on the name itself,
/// no reparse followed, identification-level security, shared for read and delete) and read through that handle by
/// @p readHeldVersion. nullopt when it can't be opened (a link on the way included), has none, or the handle is a link,
/// isn't on a disk or a CD-ROM, or a redirector serves it. Only ever given a name the reader has found to be on a local
/// volume.
[[nodiscard]] std::optional<ModuleVersion>
readLocalFileVersion(const std::wstring& ntName, const LocalFileFunctions& fns, ReadHeldVersionFn readHeldVersion);

/// readLocalFileVersion() through the system's calls and Windows::heldFileVersion().
[[nodiscard]] std::optional<ModuleVersion> readLocalFileVersion(const std::wstring& ntName);

/// The calls WindowsProcessModulesReader makes: kernel32's, or a test's fakes.
struct ProcessModuleFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using GetExitCodeProcessFn = BOOL(WINAPI*)(HANDLE, LPDWORD);
    using EnumProcessModulesExFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
    using GetModuleFileNameExFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    using GetModuleInformationFn = BOOL(WINAPI*)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
    using GetMappedFileNameFn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
    using GetLogicalDrivesFn = DWORD(WINAPI*)();
    using QueryDosDeviceFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
    using GetDriveTypeFn = UINT(WINAPI*)(LPCWSTR);
    /// Opens the file named by a local NT name and reads its version through the handle.
    using ReadFileVersionFn = std::optional<ModuleVersion> (*)(const std::wstring& ntName);

    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess;
    EnumProcessModulesExFn enumProcessModulesEx = &::EnumProcessModulesEx;
    GetModuleFileNameExFn getModuleFileNameEx = &::GetModuleFileNameExW;
    GetModuleInformationFn getModuleInformation = &::GetModuleInformation;
    GetMappedFileNameFn getMappedFileName = &::GetMappedFileNameW; // K32GetMappedFileNameW, in kernel32
    GetLogicalDrivesFn getLogicalDrives = &::GetLogicalDrives;
    QueryDosDeviceFn queryDosDevice = &::QueryDosDeviceW;
    GetDriveTypeFn getDriveType = &::GetDriveTypeW;
    ReadFileVersionFn readFileVersion = &readLocalFileVersion;
};

/// Not thread-safe: one read at a time (the Modules view runs them one at a time on a worker).
class WindowsProcessModulesReader final : public IProcessModulesReader
{
  public:
    WindowsProcessModulesReader() = default;

    /// Test seam: make every call through @p api.
    explicit WindowsProcessModulesReader(const ProcessModuleFunctions& api) : m_Api(api)
    {}

    [[nodiscard]] bool hasModules() const override;
    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& target) override;

    /// How many file versions are cached (tests: a file's version is read once).
    [[nodiscard]] std::size_t cachedVersionCount() const noexcept
    {
        return m_VersionCache.size();
    }

  private:
    /// The version of the local file whose NT name is @p ntName, read once per name and cached (nullopt: it has none).
    [[nodiscard]] std::optional<ModuleVersion> fileVersion(const std::wstring& ntName);

    ProcessModuleFunctions m_Api;
    std::unordered_map<std::wstring, std::optional<ModuleVersion>> m_VersionCache;
};

} // namespace Platform::Windows

#endif // _WIN32
