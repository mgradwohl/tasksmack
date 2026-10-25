#pragma once

// Windows IProcessModulesReader (#802): EnumProcessModulesEx(LIST_MODULES_ALL) on a handle opened
// with PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, each module's path and image size, and
// its file version from the file's VERSIONINFO, cached by path. The calls it makes are a table of
// function pointers, the system's by default, so tests can feed it fixed answers and denials
// (test_WindowsProcessModules.cpp).

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

#include <optional>
#include <string>
#include <unordered_map>

namespace Platform::Windows
{

/// The calls WindowsProcessModulesReader makes: kernel32's and version.dll's, or a test's fakes.
struct ProcessModuleFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using GetExitCodeProcessFn = BOOL(WINAPI*)(HANDLE, LPDWORD);
    using EnumProcessModulesExFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
    using GetModuleFileNameExFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    using GetModuleInformationFn = BOOL(WINAPI*)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
    using GetFileVersionInfoSizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using GetFileVersionInfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using VerQueryValueFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess;
    EnumProcessModulesExFn enumProcessModulesEx = &::EnumProcessModulesEx;
    GetModuleFileNameExFn getModuleFileNameEx = &::GetModuleFileNameExW;
    GetModuleInformationFn getModuleInformation = &::GetModuleInformation;
    GetFileVersionInfoSizeFn getFileVersionInfoSize = &::GetFileVersionInfoSizeW;
    GetFileVersionInfoFn getFileVersionInfo = &::GetFileVersionInfoW;
    VerQueryValueFn verQueryValue = &::VerQueryValueW;
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

    /// How many file versions are cached (tests: a path's version is read once).
    [[nodiscard]] std::size_t cachedVersionCount() const noexcept
    {
        return m_VersionCache.size();
    }

  private:
    /// @p path's file version, read once per path and cached (nullopt: it has none).
    [[nodiscard]] std::optional<ModuleVersion> fileVersion(const std::wstring& path);

    ProcessModuleFunctions m_Api;
    std::unordered_map<std::wstring, std::optional<ModuleVersion>> m_VersionCache;
};

} // namespace Platform::Windows

#endif // _WIN32
