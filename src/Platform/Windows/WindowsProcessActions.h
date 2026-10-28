#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "WindowsAffinityMath.h"
#include "WindowsNtQuery.h"
#include "WindowsProcessDump.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <vector>

namespace Platform
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The Win32 calls WindowsProcessActions makes to open and identify a process and to set its CPU
/// affinity, as a table so tests can make each fail as Windows would (#1881): access denied, a PID
/// reused, a refused mask. realWindowsProcessFunctions() is the real one. The calls return and set
/// GetLastError() as the APIs do.
struct WindowsProcessFunctions
{
    HANDLE(WINAPI* openProcess)(DWORD access, BOOL inherit, DWORD pid) = nullptr;
    BOOL(WINAPI* getProcessTimes)(HANDLE process, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) = nullptr;
    BOOL(WINAPI* setProcessAffinityMask)(HANDLE process, DWORD_PTR mask) = nullptr;
    /// SetProcessDefaultCpuSets(): ids null and count 0 clear the process's default CPU sets.
    BOOL(WINAPI* setProcessDefaultCpuSets)(HANDLE process, const ULONG* ids, ULONG count) = nullptr;
    /// Each processor group's maximum processor count (GetMaximumProcessorCount), in group order;
    /// empty when it can't be read. Fixed for the boot session, so read once, at construction.
    std::vector<std::uint32_t> (*readGroupSizes)() = nullptr;
    /// The physical cores (GetLogicalProcessorInformationEx, RelationProcessorCore); empty on failure.
    std::vector<ProcessorCoreRecord> (*readCores)() = nullptr;
    /// Every CPU set (GetSystemCpuSetInformation); empty on failure.
    std::vector<CpuSetRecord> (*readCpuSets)() = nullptr;
    /// PssCaptureSnapshot(), which a dump is written from (#2001); null: every dump is written live.
    Windows::PssCaptureSnapshotFn pssCaptureSnapshot = nullptr;
    /// IsProcessCritical(), asked of the verified handle before Kill, Terminate, Suspend, a priority change or an
    /// affinity change (#2556). Null, or a failed call, refuses the action: a critical process is never
    /// acted on for want of an answer.
    BOOL(WINAPI* isProcessCritical)(HANDLE process, PBOOL critical) = nullptr;
    /// GetProcessInformation(), read for ProcessProtectionLevelInfo before Kill, Terminate or Suspend (#2556).
    /// Null, or a failed call, reads as not protected: Windows refuses to end or suspend one anyway,
    /// so this only words the refusal.
    BOOL(WINAPI* getProcessInformation)(HANDLE process, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size) = nullptr;
    /// ntdll's NtSuspendProcess (#2582), through the verified handle once the guard has passed. Null:
    /// Suspend is unavailable (canStop false) and refused.
    Windows::NtProcessSuspendResumeFn ntSuspendProcess = nullptr;
    /// ntdll's NtResumeProcess (#2582). Null: Resume is unavailable (canContinue false) and refused.
    Windows::NtProcessSuspendResumeFn ntResumeProcess = nullptr;
    /// GetCurrentProcessId(): the PID Suspend refuses as TaskSmack's own before anything is opened
    /// (#2582). The real call unless a table says otherwise, so the guard is never left out; a test
    /// names a PID of its own here, so the guard fires for that PID and for no other (#2785).
    DWORD(WINAPI* getCurrentProcessId)() = &::GetCurrentProcessId;
    /// GetPriorityClass(), SetPriorityClass() and SetProcessInformation(), for Efficiency mode (#3071):
    /// the class read before it is lowered to Idle, so turning it off can restore it, and the
    /// ProcessPowerThrottling (EcoQoS) state. Null, any of them: no Efficiency mode (canSetEfficiencyMode
    /// false), so a table whose fakes hand out a real handle never reaches the real calls through them.
    DWORD(WINAPI* getPriorityClass)(HANDLE process) = nullptr;
    BOOL(WINAPI* setPriorityClass)(HANDLE process, DWORD priorityClass) = nullptr;
    BOOL(WINAPI* setProcessInformation)(HANDLE process, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size) = nullptr;
};

/// The real Win32 calls.
[[nodiscard]] WindowsProcessFunctions realWindowsProcessFunctions() noexcept;

// NOLINTEND(misc-include-cleaner)

/// Windows implementation of IProcessActions.
/// Uses TerminateProcess and related APIs. Each action opens a handle to the PID, confirms the
/// process's creation time matches the target's, and acts through that handle. The handle pins the
/// process object, so once the check passes a later reuse of the PID cannot redirect the action.
///
/// Some processes are never acted on, however TaskSmack runs: elevated, or as the elevated helper with
/// SeDebugPrivilege (#2556). PID 4 (the kernel's System process; PID 0 is refused as invalid), and any critical process (IsProcessCritical:
/// winlogon.exe, the DcomLaunch svchost.exe, whose exit bugchecks Windows), are refused Kill, Terminate,
/// a priority change, an affinity change and Suspend; a protected process is refused Kill, Terminate
/// and Suspend with a message saying so, and TaskSmack's own process is refused Suspend (#2582), which
/// would freeze it with nothing left to resume it. Each is a plain error, never permissionDenied:
/// running as administrator, which "Retry as administrator" offers, doesn't make it safe. Every action
/// Windows refuses for want of access (ERROR_ACCESS_DENIED from OpenProcess or the call itself, and
/// STATUS_ACCESS_DENIED from NtSuspendProcess or NtResumeProcess) sets permissionDenied (#2582, #2602), so
/// BrokeredProcessActions can retry it through the elevated helper; except for a protected process,
/// whose refused open says it is protected, with no retry, since an administrator is refused too.
class WindowsProcessActions : public IProcessActions
{
  public:
    WindowsProcessActions();
    /// As above, making its Win32 calls through @p functions (tests: calls that fail on demand).
    explicit WindowsProcessActions(WindowsProcessFunctions functions);
    ~WindowsProcessActions() override = default;

    WindowsProcessActions(const WindowsProcessActions&) = delete;
    WindowsProcessActions& operator=(const WindowsProcessActions&) = delete;
    WindowsProcessActions(WindowsProcessActions&&) = default;
    WindowsProcessActions& operator=(WindowsProcessActions&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    /// Suspend (#2582): NtSuspendProcess on the verified handle, every thread at once, as Resource
    /// Monitor and Process Explorer do. Refused for PID 4, TaskSmack's own process, and a critical or
    /// protected process. Windows counts suspensions, so a process suspended twice needs two resumes.
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    /// Resume (#2582): NtResumeProcess on the verified handle. Not guarded: letting a process run again
    /// can't stop Windows.
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;
    /// Windows has no ionice equivalent (#803): refused, and canSetIoPriority is false.
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) override;
    /// Refused, as setIoPriority() is.
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
    /// One processor group: SetProcessAffinityMask(), which every thread, and every thread started
    /// later, follows. Several: SetProcessDefaultCpuSets(), since one affinity mask can't span groups
    /// (#1881); every CPU chosen clears the default CPU sets instead. CPU sets are a preference: threads
    /// given an affinity or CPU sets of their own, and a hard affinity mask, take precedence, so the
    /// success carries a successNote saying the choice was set as preferred CPUs (#2173).
    [[nodiscard]] ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus) override;
    [[nodiscard]] CpuAffinityLayout affinityLayout() const override;
    /// Efficiency mode (#3071), as Task Manager's: on sets ProcessPowerThrottling's execution-speed
    /// throttling (EcoQoS: the efficient cores, lower clocks) and the Idle priority class, remembering the
    /// class it had (sharedEfficiencyModePriorities()); off hands power throttling back to Windows and,
    /// while the process is still at Idle, restores that class, or Normal when it isn't known. Refused for
    /// PID 4 and for a protected or critical process, through the same identity check and guard as a
    /// priority change; access denied is permissionDenied, so the elevated helper can retry it. A failure
    /// to lower the class undoes the throttling, so a refused "on" changes nothing.
    [[nodiscard]] ProcessActionResult setEfficiencyMode(const ProcessTarget& target, bool on) override;
    /// A full-memory minidump through dbghelp's MiniDumpWriteDump (#807), as Task Manager writes one;
    /// see Windows::writeProcessDump(). Opens the process through the same identity check as every
    /// action, and says why when that is refused: a protected process, one elevated or another user's.
    /// From a PssCaptureSnapshot() clone where it can take one (#2001), so the process pauses only for
    /// the snapshot; else live, saying why (a process that refuses the snapshot's access, a failed
    /// capture).
    [[nodiscard]] ProcessDumpResult
    createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override;
    /// GetDiskFreeSpaceEx() on the folder, and the process's private bytes through the identity check.
    [[nodiscard]] DumpSpace dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory) override;
    /// IFileOpenDialog with FOS_PICKFOLDERS (Windows::pickFolder()).
    [[nodiscard]] FolderChoice chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow) override;

  private:
    /// Helper to terminate a process with given exit code
    [[nodiscard]] ProcessActionResult terminateProcess(const ProcessTarget& target, uint32_t exitCode) const;
    /// stop() (@p suspend) or resume().
    [[nodiscard]] ProcessActionResult suspendOrResume(const ProcessTarget& target, bool suspend) const;

    WindowsProcessFunctions m_Fns;
    std::vector<std::uint32_t> m_GroupSizes; // Each processor group's maximum processor count, read at construction
};

} // namespace Platform
