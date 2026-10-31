#pragma once

// Start at sign-in on Windows (#3093, part 2): the per-user Run key's "TaskSmack" value,
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, as the Settings checkbox turns it on and off.
// Per-user, so no elevation; only that one value is ever read, written or deleted. The calls are a table
// of function pointers, advapi32's by default, so tests drive every path with fakes; the one real-registry
// test points the key at a scratch key under HKCU\Software\TaskSmack.Tests and deletes it afterwards.
// Never the real Run key from a test.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "Platform/IStartAtLogin.h"

#include <expected>
#include <string>
#include <string_view>

#include <shlobj.h>

namespace Platform
{

namespace Windows
{

/// The Run key, under HKEY_CURRENT_USER, and TaskSmack's value in it.
inline constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* RUN_VALUE_NAME = L"TaskSmack";

/// The calls WindowsStartAtLogin makes: advapi32's, kernel32's and shell32's, or a test's fakes.
struct StartAtLoginFunctions
{
    using RegOpenKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    using RegCreateKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
    using RegQueryValueExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    using RegSetValueExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
    using RegDeleteValueFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR);
    using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
    using GetModuleFileNameFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
    /// SHGetKnownFolderPath: the path is allocated with CoTaskMemAlloc, and freed here with CoTaskMemFree.
    using KnownFolderPathFn = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);

    RegOpenKeyExFn openKey = &::RegOpenKeyExW;
    RegCreateKeyExFn createKey = &::RegCreateKeyExW;
    RegQueryValueExFn queryValue = &::RegQueryValueExW;
    RegSetValueExFn setValue = &::RegSetValueExW;
    RegDeleteValueFn deleteValue = &::RegDeleteValueW;
    RegCloseKeyFn closeKey = &::RegCloseKey;
    GetModuleFileNameFn moduleFileName = &::GetModuleFileNameW;
    KnownFolderPathFn knownFolderPath = &::SHGetKnownFolderPath;
};

} // namespace Windows

/// The Run value, read and written through the function table. Stateless beyond it, so safe from any thread.
class WindowsStartAtLogin final : public IStartAtLogin
{
  public:
    /// @param key        The key, under HKEY_CURRENT_USER: Windows::RUN_KEY, or a test's scratch key.
    /// @param valueName  The value: Windows::RUN_VALUE_NAME.
    explicit WindowsStartAtLogin(std::wstring key = Windows::RUN_KEY,
                                 std::wstring valueName = Windows::RUN_VALUE_NAME,
                                 Windows::StartAtLoginFunctions api = {});

    [[nodiscard]] std::expected<StartAtLoginFacts, std::string> read() override;
    [[nodiscard]] std::expected<void, std::string> write(std::string_view command) override;
    [[nodiscard]] std::expected<void, std::string> remove() override;

  private:
    std::wstring m_Key;
    std::wstring m_ValueName;
    Windows::StartAtLoginFunctions m_Api;
};

} // namespace Platform

#endif // _WIN32
