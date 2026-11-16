/// @file WindowsFakeProcessIdentity.h
/// @brief One fake of the open-and-check-identity calls a per-process Windows reader makes (#3849): OpenProcess,
/// CloseHandle, GetProcessTimes and GetExitCodeProcess, shared by the module and environment readers' tests.
///
/// Each test keeps its own fake state (its FakeProcess, reached through a function the template is given), which
/// must have: openError (non-zero: OpenProcess fails with it), creationTicks (what GetProcessTimes reports),
/// exitCode (STILL_ACTIVE while running), and the opens and closes counters. OpenProcess expects exactly @p Access
/// and @p Pid, so a reader that asks for more rights, or for another process, fails the test. The handle is a token,
/// never a real one.

#pragma once

#ifdef _WIN32

#include <gtest/gtest.h>

#include <cstdint>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace TestMocks
{

template<typename Fake, Fake& (*State)(), std::int32_t Pid, DWORD Access> struct FakeProcessIdentity
{
    /// The token OpenProcess hands back, and every other fake expects.
    [[nodiscard]] static HANDLE handle()
    {
        static int token = 0;
        return &token;
    }

    static HANDLE WINAPI openProcess(DWORD access, BOOL /*inherit*/, DWORD pid)
    {
        EXPECT_EQ(access, Access);
        EXPECT_EQ(pid, static_cast<DWORD>(Pid));
        if (State().openError != 0)
        {
            SetLastError(State().openError);
            return nullptr;
        }
        ++State().opens;
        return handle();
    }

    static BOOL WINAPI closeHandle(HANDLE process)
    {
        EXPECT_EQ(process, handle());
        ++State().closes;
        return TRUE;
    }

    static BOOL WINAPI
    getProcessTimes(HANDLE /*process*/, LPFILETIME creation, LPFILETIME /*exit*/, LPFILETIME /*kernel*/, LPFILETIME /*user*/)
    {
        creation->dwLowDateTime = static_cast<DWORD>(State().creationTicks & 0xFFFFFFFFU);
        creation->dwHighDateTime = static_cast<DWORD>(State().creationTicks >> 32U);
        return TRUE;
    }

    static BOOL WINAPI getExitCodeProcess(HANDLE /*process*/, LPDWORD code)
    {
        *code = State().exitCode;
        return TRUE;
    }
};

} // namespace TestMocks

#endif // _WIN32
