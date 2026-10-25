#pragma once

// The one place the Windows probes resolve ntdll's NtQuerySystemInformation(Ex) (#1183), the
// actions its NtSuspendProcess and NtResumeProcess (#2582), and the environment reader its
// NtQueryInformationProcess (#2569). ntdll.dll is already loaded in every
// process, so it is found with GetModuleHandleW, never loaded from a search path. Each lookup
// is a function-local static initialised by a lambda, so it runs once, thread-safely, whichever
// probe asks first; and, these being inline functions, every translation unit shares that one static.

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

#include "Platform/Windows/WindowsProcAddress.h"

namespace Platform::Windows
{

/// NtQuerySystemInformation. The class is a ULONG (SYSTEM_INFORMATION_CLASS is an enum with the same
/// representation); the result is an NTSTATUS, which is a LONG.
using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG systemInformationClass,
                                                PVOID systemInformation,
                                                ULONG systemInformationLength,
                                                PULONG returnLength);

/// NtQuerySystemInformationEx. For SystemProcessorPerformanceInformation the input buffer is the
/// USHORT processor group to report (#1107).
using NtQuerySystemInformationExFn = LONG(NTAPI*)(ULONG systemInformationClass,
                                                  PVOID inputBuffer,
                                                  ULONG inputBufferLength,
                                                  PVOID systemInformation,
                                                  ULONG systemInformationLength,
                                                  PULONG returnLength);

/// NtSuspendProcess or NtResumeProcess (#2582): every thread of the process, as one call, so a thread
/// started meanwhile isn't missed. The handle needs PROCESS_SUSPEND_RESUME; the result is an NTSTATUS.
using NtProcessSuspendResumeFn = LONG(NTAPI*)(HANDLE process);

/// NtQueryInformationProcess, its class passed as a ULONG (PROCESSINFOCLASS's representation, so the
/// classes winternl.h leaves out need no enum cast); the result is an NTSTATUS. ProcessBasicInformation
/// (class 0) gives a process's PEB address (#2569).
using NtQueryInformationProcessFn = LONG(NTAPI*)(
    HANDLE process, ULONG processInformationClass, PVOID processInformation, ULONG processInformationLength, PULONG returnLength);

/// Look up `name` in ntdll.dll, which every process has loaded; null if it is missing.
template<typename Fn> [[nodiscard]] Fn ntdllProc(const char* name) noexcept
{
    return getProcAddress<Fn>(GetModuleHandleW(L"ntdll.dll"), name);
}

/// ntdll's NtQuerySystemInformation, or null if it cannot be found. Resolved once.
[[nodiscard]] inline NtQuerySystemInformationFn ntQuerySystemInformation() noexcept
{
    static const auto fn = [] noexcept
    {
        return ntdllProc<NtQuerySystemInformationFn>("NtQuerySystemInformation");
    }();
    return fn;
}

/// ntdll's NtQuerySystemInformationEx, or null if it cannot be found. Resolved once.
[[nodiscard]] inline NtQuerySystemInformationExFn ntQuerySystemInformationEx() noexcept
{
    static const auto fn = [] noexcept
    {
        return ntdllProc<NtQuerySystemInformationExFn>("NtQuerySystemInformationEx");
    }();
    return fn;
}

/// ntdll's NtSuspendProcess, or null if it cannot be found. Resolved once.
[[nodiscard]] inline NtProcessSuspendResumeFn ntSuspendProcess() noexcept
{
    static const auto fn = [] noexcept
    {
        return ntdllProc<NtProcessSuspendResumeFn>("NtSuspendProcess");
    }();
    return fn;
}

/// ntdll's NtResumeProcess, or null if it cannot be found. Resolved once.
[[nodiscard]] inline NtProcessSuspendResumeFn ntResumeProcess() noexcept
{
    static const auto fn = [] noexcept
    {
        return ntdllProc<NtProcessSuspendResumeFn>("NtResumeProcess");
    }();
    return fn;
}

/// ntdll's NtQueryInformationProcess, or null if it cannot be found. Resolved once.
[[nodiscard]] inline NtQueryInformationProcessFn ntQueryInformationProcess() noexcept
{
    static const auto fn = [] noexcept
    {
        return ntdllProc<NtQueryInformationProcessFn>("NtQueryInformationProcess");
    }();
    return fn;
}

} // namespace Platform::Windows

#endif // _WIN32
