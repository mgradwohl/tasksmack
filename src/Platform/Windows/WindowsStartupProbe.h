#pragma once

#include "Platform/IStartupProbe.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <shlwapi.h>
// clang-format on

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

namespace Windows
{

/// The registry, known-folder and package calls WindowsStartupProbe makes (#1722, #1798): advapi32's,
/// shell32's, kernel32's and shlwapi's by default, or a test's fakes, so the Run keys, the Startup
/// folders, their StartupApproved state and packaged apps' startup tasks can be tested without reading
/// (or writing) the real ones. Every key opened is closed through closeKey.
struct StartupProbeFunctions
{
    using RegOpenKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    using RegQueryInfoKeyFn =
        LSTATUS(WINAPI*)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
    using RegEnumValueFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    using RegEnumKeyExFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
    using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
    /// SHGetKnownFolderPath: the path is allocated with CoTaskMemAlloc, and the caller frees it.
    using KnownFolderPathFn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
    /// GetPackagesByPackageFamily: the full names of a family's packages installed for the current user.
    using PackagesByFamilyFn = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR*, UINT32*, WCHAR*);
    /// GetPackagePathByFullName: a package's install folder.
    using PackagePathFn = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR);
    /// SHLoadIndirectString: an "@{PackageFullName?ms-resource://...}" string from the package's resources.
    using LoadIndirectStringFn = HRESULT(WINAPI*)(PCWSTR, PWSTR, UINT, void**);

    RegOpenKeyExFn openKey = &::RegOpenKeyExW;
    RegQueryInfoKeyFn queryInfoKey = &::RegQueryInfoKeyW;
    RegEnumValueFn enumValue = &::RegEnumValueW;
    RegEnumKeyExFn enumKey = &::RegEnumKeyExW;
    RegCloseKeyFn closeKey = &::RegCloseKey;
    KnownFolderPathFn knownFolderPath = &::SHGetKnownFolderPath;
    PackagesByFamilyFn packagesByFamily = &::GetPackagesByPackageFamily;
    PackagePathFn packagePath = &::GetPackagePathByFullName;
    LoadIndirectStringFn loadIndirectString = &::SHLoadIndirectString;
};

/// One windows.startupTask a package manifest declares (#1798). Strings are as written in the
/// manifest: a name may be an "ms-resource:" reference, and the executable is relative to the package.
struct PackageStartupTask
{
    std::string taskId;
    /// The task's DisplayName, else its Application's VisualElements DisplayName, else empty.
    std::string displayName;
    /// The extension's Executable, else its Application's.
    std::string executable;
    bool enabledByDefault = false; ///< The StartupTask's Enabled attribute.
    /// Its Application's VisualElements Square44x44Logo (#1800), else the package's Logo; relative
    /// to the package, e.g. "Assets\Square44x44Logo.png".
    std::string logo;
};

/// What WindowsStartupProbe reads from a package's AppxManifest.xml.
struct PackageManifest
{
    std::string displayName;          ///< Package/Properties/DisplayName.
    std::string publisherDisplayName; ///< Package/Properties/PublisherDisplayName.
    std::string logo;                 ///< Package/Properties/Logo (the Store logo), relative to the package.
    std::vector<PackageStartupTask> startupTasks;
};

} // namespace Windows

/// Windows startup entries (#801), read-only and without elevation.
///
/// Reads the Run and RunOnce keys (HKCU, HKLM and HKLM's 32-bit view), the user and all-users
/// Startup folders (shortcut targets through IShellLinkW, with COM initialised for the duration of
/// each enumerate() on the calling thread), the enabled state and disable time from
/// Explorer\StartupApproved, each executable's version-resource CompanyName and FileDescription (the
/// publisher and the name shown, #1873; cached per file until its last-write time changes), and the current user's packaged apps' startup
/// tasks (#1798): those whose state is recorded under StartupMath::PACKAGED_TASKS_KEY and whose installed package's manifest still declares
/// them, named from the manifest (cached per package version).
class WindowsStartupProbe : public IStartupProbe
{
  public:
    WindowsStartupProbe();
    /// Make every registry and known-folder call through @p functions (tests, #1722).
    explicit WindowsStartupProbe(const Windows::StartupProbeFunctions& functions);
    ~WindowsStartupProbe() override;

    WindowsStartupProbe(const WindowsStartupProbe&) = delete;
    WindowsStartupProbe& operator=(const WindowsStartupProbe&) = delete;
    WindowsStartupProbe(WindowsStartupProbe&&) = delete;
    WindowsStartupProbe& operator=(WindowsStartupProbe&&) = delete;

    [[nodiscard]] StartupCapabilities capabilities() const override;
    [[nodiscard]] std::vector<StartupEntry> enumerate() override;
    /// The newest sign-in's measurement from the user's WDI StartupInfo files (WindowsStartupImpact.h, #3073).
    [[nodiscard]] StartupImpactReading readImpact() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

namespace Windows
{

/// `text` with %VARIABLE% references expanded by ExpandEnvironmentStringsW; unknown variables are
/// left as written. UTF-8 in and out.
[[nodiscard]] std::string expandEnvironmentStrings(std::string_view text);

/// The executable `commandLine` starts, as WindowsStartupProbe resolves it: environment variables
/// expanded, then StartupMath::executableFromCommandLine, then a bare file name ("ctfmon.exe")
/// searched for on the search path. Empty when there is none.
[[nodiscard]] std::string resolveStartupExecutable(std::string_view commandLine);

/// A shortcut (.lnk) file's target and arguments as one command line ("target" args), environment
/// variables as stored; nullopt when it can't be loaded or has no file-system target (e.g. an
/// advertised installer shortcut). COM must be initialised on the calling thread.
[[nodiscard]] std::optional<std::string> readShortcutCommand(const std::filesystem::path& shortcut);

/// The names and windows.startupTask extensions in a package manifest (AppxManifest.xml), whatever the
/// namespace prefixes; nullopt when @p xml isn't well-formed. COM need not be initialised.
[[nodiscard]] std::optional<PackageManifest> parsePackageManifest(std::string_view xml);

} // namespace Windows

} // namespace Platform
