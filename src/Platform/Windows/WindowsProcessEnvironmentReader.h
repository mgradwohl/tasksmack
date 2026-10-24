#pragma once

// Windows IProcessEnvironmentReader (#2569): the target's environment block, read out of its memory.
// OpenProcess with PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ (its creation time confirms
// the target's identity, WindowsProcessIdentity.h), NtQueryInformationProcess(ProcessBasicInformation)
// for the address of its PEB, then ReadProcessMemory for PEB::ProcessParameters,
// RTL_USER_PROCESS_PARAMETERS::Environment and ::EnvironmentSize, and the block itself: UTF-16
// "NAME=value" strings, each ended by a NUL, the last followed by a second NUL. The block is the
// process's live one (SetEnvironmentVariable writes there), so later changes show, unlike Linux's
// /proc/[pid]/environ, which keeps the block the process started with. The calls it makes are a table
// of function pointers, the system's by default, so tests feed it a fake process's memory and denials
// (test_WindowsProcessEnvironmentReader.cpp). Values are never logged (#179).
//
// The layouts read are those of the reader's own pointer size (64-bit for the x64 and ARM64 builds).
// A process of the other size -- a 32-bit (WOW64) process seen from the 64-bit build -- keeps its
// environment behind a PEB of the 32-bit layout, which this first cut does not read: it reports
// Failed, saying so in the detail. (Such a process has a 64-bit PEB of the layout read here as well,
// but that one holds only the block it started with, which would pass for current.) A follow-up can
// read the 32-bit PEB, found with NtQueryInformationProcess(ProcessWow64Information). Reading as an
// administrator through the elevated helper (canReadAuthorized(), #2517) is a later step too.

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

#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/Windows/WindowsNtQuery.h"

#include <cstddef>

namespace Platform::Windows
{

/// The most of an environment block read, in bytes: a corrupted or hostile EnvironmentSize, or a block
/// with no terminator, can't make TaskSmack read without limit. A real block is a few KiB (Windows caps
/// a single variable at 32 KiB); a block longer than this is cut here and the result says truncated.
inline constexpr std::size_t ENVIRONMENT_BLOCK_READ_CAP_BYTES = std::size_t{1024} * 1024;

/// The most variables one read passes on; past it the rest are dropped and the result says truncated.
inline constexpr std::size_t ENVIRONMENT_VARIABLE_CAP = 4096;

/// The calls WindowsProcessEnvironmentReader makes: kernel32's and ntdll's, or a test's fakes. The
/// handle openProcess returns is closed through closeHandle, so a fake handle never reaches the real
/// CloseHandle. isWow64Process2 is also asked about this process (GetCurrentProcess()); the pointer
/// size each answer's machine implies (x86 and ARM32 are 32-bit; UNKNOWN, AMD64 and ARM64 are 64-bit)
/// tells the reader's own from the target's.
struct ProcessEnvironmentFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using GetExitCodeProcessFn = BOOL(WINAPI*)(HANDLE, LPDWORD);
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    using ReadProcessMemoryFn = BOOL(WINAPI*)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T*);

    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess;
    IsWow64Process2Fn isWow64Process2 = &::IsWow64Process2;
    /// ntdll's, resolved once (WindowsNtQuery.h). Null if ntdll lacks it, when every read is Failed.
    NtQueryInformationProcessFn ntQueryInformationProcess = Windows::ntQueryInformationProcess();
    ReadProcessMemoryFn readProcessMemory = &::ReadProcessMemory;
};

/// Thread-safe: it holds only its table of calls. The Environment section runs reads on its worker,
/// one at a time (LazyBackgroundRead), never on the UI thread.
class WindowsProcessEnvironmentReader final : public IProcessEnvironmentReader
{
  public:
    WindowsProcessEnvironmentReader() = default;

    /// Test seam: make every call through @p api.
    explicit WindowsProcessEnvironmentReader(const ProcessEnvironmentFunctions& api) : m_Api(api)
    {}

    [[nodiscard]] bool hasEnvironment() const override;
    [[nodiscard]] EnvironmentReadResult readEnvironment(const ProcessTarget& target) override;

  private:
    ProcessEnvironmentFunctions m_Api;
};

} // namespace Platform::Windows

#endif // _WIN32
