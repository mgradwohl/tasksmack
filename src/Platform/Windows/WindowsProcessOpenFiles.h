#pragma once

// Windows IProcessOpenFilesReader (#183): the process's File handles, from the system handle table.
//
//   1. OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION), and the start time checked
//      against the target's (a protected or elevated process refuses: PermissionDenied).
//   2. NtQuerySystemInformation(SystemExtendedHandleInformation): every handle in the system, kept
//      only for the target PID and the File object type. The File type's index is learned once, from
//      a handle of TaskSmack's own on "NUL" found in the same table.
//   3. Each handle DUPLICATE_SAME_ACCESS-duplicated into TaskSmack (never closed or changed in the
//      target), typed with GetFileType(), and named with GetFinalPathNameByHandleW() only when it is a
//      disk file: pipes and character devices (consoles) are where a name query can block forever.
//      Handles whose GrantedAccess is a mask known to belong to such blocking handles are not touched.
//   4. Steps 3 run on a separate namer thread that the read waits on with a per-handle timeout. A
//      handle that does not answer abandons the namer (it finishes, closing its duplicate, if the
//      handle ever answers), the rest are listed unnamed, and the result says so. That handle is
//      skipped on later reads, and at most MAX_STUCK_NAMERS namers are ever left waiting.
//
// The calls it makes are a table of function pointers, the system's by default, so tests feed it
// fixed handle tables, denials and a handle that hangs (test_WindowsProcessOpenFiles.cpp).

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
#include "Platform/IProcessOpenFiles.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{

/// SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX: one entry of the SystemExtendedHandleInformation table.
struct SystemHandleEntry
{
    void* object = nullptr; ///< Zero for an unprivileged caller on current Windows.
    ULONG_PTR uniqueProcessId = 0;
    ULONG_PTR handleValue = 0;
    ULONG grantedAccess = 0;
    USHORT creatorBackTraceIndex = 0;
    USHORT objectTypeIndex = 0;
    ULONG handleAttributes = 0;
    ULONG reserved = 0;
};

/// SystemExtendedHandleInformation's class number for NtQuerySystemInformation.
inline constexpr ULONG SYSTEM_EXTENDED_HANDLE_INFORMATION = 64;

/// The handles of @p pid in @p snapshot (a SYSTEM_HANDLE_INFORMATION_EX: a count, a reserved word,
/// then the entries). A count larger than the buffer holds is clamped to what it holds.
[[nodiscard]] std::vector<SystemHandleEntry> handlesOfProcess(std::span<const std::byte> snapshot, ULONG_PTR pid);

/// Whether a handle with @p grantedAccess is one whose queries are known to block (synchronous named
/// pipes): it is listed but never touched. The well-known list also has 0x0012019F, but that is plain
/// GENERIC_READ | GENERIC_WRITE, the mask of most files opened for writing; those are typed first and
/// named only as disk files, under the timeout.
[[nodiscard]] constexpr bool isHangProneAccess(ULONG grantedAccess) noexcept
{
    return grantedAccess == 0x001A019F || grantedAccess == 0x00120189 || grantedAccess == 0x00100000;
}

/// The calls WindowsProcessOpenFilesReader makes: the system's, or a test's fakes.
struct ProcessOpenFilesFunctions
{
    using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using DuplicateHandleFn = BOOL(WINAPI*)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
    using GetFileTypeFn = DWORD(WINAPI*)(HANDLE);
    using GetFinalPathNameByHandleFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
    using CreateFileFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using GetCurrentProcessIdFn = DWORD(WINAPI*)();

    NtQuerySystemInformationFn ntQuerySystemInformation = nullptr; ///< ntdll's, looked up when null
    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    DuplicateHandleFn duplicateHandle = &::DuplicateHandle;
    GetFileTypeFn getFileType = &::GetFileType;
    GetFinalPathNameByHandleFn getFinalPathNameByHandle = &::GetFinalPathNameByHandleW;
    CreateFileFn createFile = &::CreateFileW;
    GetCurrentProcessIdFn getCurrentProcessId = &::GetCurrentProcessId;
};

/// How long the read waits for one handle's type and name before giving up on the namer.
inline constexpr std::chrono::milliseconds DEFAULT_HANDLE_NAME_TIMEOUT{500};

/// The most namers ever left waiting on handles that did not answer; past it, reads list handles unnamed.
inline constexpr std::size_t MAX_STUCK_NAMERS = 4;

/// Not thread-safe: one read at a time (the Open files view runs them one at a time on a worker).
class WindowsProcessOpenFilesReader final : public IProcessOpenFilesReader
{
  public:
    WindowsProcessOpenFilesReader();

    /// Test seam: make every call through @p api, waiting @p handleTimeout for each handle.
    explicit WindowsProcessOpenFilesReader(const ProcessOpenFilesFunctions& api,
                                           std::chrono::milliseconds handleTimeout = DEFAULT_HANDLE_NAME_TIMEOUT);

    ~WindowsProcessOpenFilesReader() override;
    WindowsProcessOpenFilesReader(const WindowsProcessOpenFilesReader&) = delete;
    WindowsProcessOpenFilesReader& operator=(const WindowsProcessOpenFilesReader&) = delete;
    WindowsProcessOpenFilesReader(WindowsProcessOpenFilesReader&&) = delete;
    WindowsProcessOpenFilesReader& operator=(WindowsProcessOpenFilesReader&&) = delete;

    [[nodiscard]] bool hasOpenFiles() const override;
    [[nodiscard]] OpenFilesReadResult readOpenFiles(const ProcessTarget& target) override;
    [[nodiscard]] std::size_t handlesScanned() const noexcept override;

    /// Namers still waiting on a handle that did not answer (tests).
    [[nodiscard]] std::size_t stuckNamerCount();

    struct NamingState;

  private:
    /// The whole system handle table into m_Snapshot; an error message on failure.
    [[nodiscard]] std::optional<std::string> querySnapshot();
    /// The File object type's index, learned from a handle of our own; nullopt if it can't be.
    [[nodiscard]] std::optional<USHORT> fileTypeIndex();
    /// Frees m_Snapshot between reads.
    void releaseSnapshot() noexcept;

    ProcessOpenFilesFunctions m_Api;
    std::chrono::milliseconds m_HandleTimeout;
    std::vector<std::byte> m_Snapshot;   // only during a read
    std::size_t m_SnapshotBytesHint = 0; // the size the table last needed, to start the next read there
    std::optional<USHORT> m_FileTypeIndex;
    std::vector<std::shared_ptr<NamingState>> m_StuckNamers;
    std::set<std::pair<ULONG_PTR, ULONG_PTR>> m_HungHandles; // (pid, handle) that did not answer: skipped
    std::atomic<std::size_t> m_HandlesScanned{0};            // the read in flight's progress, read by the UI
};

} // namespace Platform::Windows

#endif // _WIN32
