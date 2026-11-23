#pragma once

/// @file WindowsSandbox.h
/// @brief Whether this test process runs in Windows Sandbox (#4827), for the real-system tests that assume a normal
/// Windows install. Sandbox serves the system files from the host over VSMB, so this process's ntdll was mapped from
/// a \Device\vmsmb\... name, which has no drive-letter form; Microsoft's signatures don't verify there, the system
/// files aren't TrustedInstaller's and Authenticated Users may change C:\. A test whose assertion one of those breaks
/// skips it in Sandbox, with GTEST_SKIP() and a reason. tests/tools/test_nsis_upgrade.py's in_windows_sandbox()
/// asks the same question the same way.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // GetMappedFileNameW
// clang-format on

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace TestSupport
{

/// Whether @p ntName, a mapped file's NT name, is served over VSMB (\Device\vmsmb\...), compared ignoring case.
[[nodiscard]] inline bool isVsmbName(std::wstring_view ntName) noexcept
{
    constexpr std::wstring_view VSMB_PREFIX = L"\\Device\\vmsmb\\";
    return ntName.size() >= VSMB_PREFIX.size() && std::ranges::equal(
                                                      ntName.substr(0, VSMB_PREFIX.size()),
                                                      VSMB_PREFIX,
                                                      {},
                                                      [](wchar_t c) { return std::towlower(c); },
                                                      [](wchar_t c) { return std::towlower(c); });
}

/// Whether this process runs in Windows Sandbox: its ntdll image was mapped from a file served over VSMB. Read once;
/// a name that can't be read says no, so a normal install never skips.
[[nodiscard]] inline bool isWindowsSandbox()
{
    static const bool sandbox = []
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return false;
        }
        std::wstring ntName(32768, L'\0');
        const DWORD length = GetMappedFileNameW(GetCurrentProcess(), ntdll, ntName.data(), static_cast<DWORD>(ntName.size()));
        ntName.resize(length);
        return isVsmbName(ntName);
    }();
    return sandbox;
}

} // namespace TestSupport
#endif
