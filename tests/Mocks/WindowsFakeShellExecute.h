/// @file WindowsFakeShellExecute.h
/// @brief One fake ShellExecuteExW for the tests that start a program through it (#3849): HelperLauncher's "runas"
/// launch of the helper and ElevatedRestart's restart of TaskSmack elevated.
///
/// It records what it was asked and answers as the test sets: a failure with failWith, or success handing back this
/// process (or no process, with noProcess), so no UAC prompt is ever raised and nothing runs elevated. The process
/// handle carries SYNCHRONIZE and PROCESS_QUERY_LIMITED_INFORMATION, enough to wait on it and read its exit code.
/// One fake per test binary (instance()); a test resets it in SetUp.

#pragma once

#ifdef _WIN32

#include <string>

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

namespace TestMocks
{

/// What the fake ShellExecuteExW was asked, and how it answers.
struct FakeShellExecute
{
    int calls = 0;
    std::wstring verb;
    std::wstring file;
    std::wstring parameters;
    ULONG mask = 0;
    int show = -1;
    HWND window = nullptr;
    DWORD failWith = ERROR_SUCCESS; ///< ERROR_SUCCESS: succeed, handing back this process
    bool noProcess = false;         ///< succeed without a process

    [[nodiscard]] static FakeShellExecute& instance()
    {
        static FakeShellExecute fake;
        return fake;
    }

    /// A fresh fake that fails with @p failWith (ERROR_SUCCESS: succeeds).
    static FakeShellExecute& reset(DWORD failWith = ERROR_SUCCESS)
    {
        instance() = FakeShellExecute{};
        instance().failWith = failWith;
        return instance();
    }

    static BOOL WINAPI shellExecuteEx(SHELLEXECUTEINFOW* info)
    {
        FakeShellExecute& fake = instance();
        ++fake.calls;
        fake.verb = info->lpVerb != nullptr ? info->lpVerb : L"";
        fake.file = info->lpFile != nullptr ? info->lpFile : L"";
        fake.parameters = info->lpParameters != nullptr ? info->lpParameters : L"";
        fake.mask = info->fMask;
        fake.show = info->nShow;
        fake.window = info->hwnd;
        if (fake.failWith != ERROR_SUCCESS)
        {
            SetLastError(fake.failWith);
            return FALSE;
        }
        info->hProcess =
            fake.noProcess ? nullptr : OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
        return TRUE;
    }
};

} // namespace TestMocks

#endif // _WIN32
