#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IoPriorityTypes.h"
#include "WindowsAffinityMath.h"
#include "WindowsFileLocation.h"
#include "WindowsNtQuery.h"
#include "WindowsProcessDump.h"
#include "WindowsRunProgram.h"
#include "WindowsTcpClose.h"

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
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace Platform
{

class EfficiencyModePriorities;

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
    /// setPriority() sets every class through setPriorityClass (the real call where it is null), and Realtime
    /// only with both it and getPriorityClass, which reads the class back (#3429; canSetRealtimePriority).
    DWORD(WINAPI* getPriorityClass)(HANDLE process) = nullptr;
    BOOL(WINAPI* setPriorityClass)(HANDLE process, DWORD priorityClass) = nullptr;
    BOOL(WINAPI* setProcessInformation)(HANDLE process, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size) = nullptr;
    /// OpenThread(), GetProcessIdOfThread(), GetThreadTimes(), SuspendThread(), ResumeThread() and TerminateThread(), for one
    /// thread's Suspend, Resume and End (#3069). The thread handle is closed with the real CloseHandle, so a
    /// fake OpenThread hands out a real handle that can change nothing (a thread of the test's own, opened for
    /// query only). Null, any of them: no thread actions (canActOnThreads false).
    HANDLE(WINAPI* openThread)(DWORD access, BOOL inherit, DWORD tid) = nullptr;
    DWORD(WINAPI* getProcessIdOfThread)(HANDLE thread) = nullptr;
    DWORD(WINAPI* suspendThread)(HANDLE thread) = nullptr;
    DWORD(WINAPI* resumeThread)(HANDLE thread) = nullptr;
    BOOL(WINAPI* terminateThread)(HANDLE thread, DWORD exitCode) = nullptr;
    /// GetThreadTimes(): the thread's creation time, checked against the one the user confirmed (#3069).
    BOOL(WINAPI* getThreadTimes)(HANDLE thread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) = nullptr;
    /// QueryFullProcessImageNameW(), on the verified handle: the executable Open file location shows (#3426).
    BOOL(WINAPI* queryFullProcessImageName)(HANDLE process, DWORD flags, LPWSTR path, PDWORD size) = nullptr;
    /// Explorer asked to show that executable selected (#3426). Null queryFullProcessImageName, or no
    /// selectInFolder and no startProcess here: no Open file location (canOpenFileLocation false), as in
    /// TaskSmackHelper.exe, which never serves one (BrokeredProcessActions keeps it the user's own).
    Windows::FileLocationFunctions fileLocation{};
    /// ShellExecuteExW and the elevation check, for Run new task (#3427). Null shellExecute: no Run new task
    /// (canRunProgram false), as in TaskSmackHelper.exe, which never serves one (BrokeredProcessActions keeps
    /// it the user's own), and in a test's table, so no test starts a program or raises UAC.
    Windows::RunProgramFunctions runProgram{};
    /// ntdll's NtQueryInformationProcess and NtSetInformationProcess, for ProcessIoPriority (#3362), on the
    /// verified handle. Null, either: no I/O priority (canSetIoPriority false), and nothing is opened.
    Windows::NtQueryInformationProcessFn ntQueryInformationProcess = nullptr;
    Windows::NtSetInformationProcessFn ntSetInformationProcess = nullptr;
    /// Enables SeIncreaseBasePriorityPrivilege on TaskSmack's own token when it is elevated, before High is set
    /// (#3362; Windows::enableIncreaseBasePriorityPrivilegeIfElevated()); true when it is enabled. Null: not
    /// tried, and Windows decides by the token as it stands, as in a test's table.
    bool (*enableIncreaseBasePriority)() = nullptr;
    /// The IPv4 TCP table and SetTcpEntry, for Close connection (#3358). Null setTcpEntry: no Close connection
    /// (canCloseConnections false), as in a test's table, so no test resets a connection it didn't make.
    Windows::TcpCloseFunctions tcpClose{};
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
/// a priority or I/O priority change, an affinity change and Suspend; a protected process is refused Kill, Terminate
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
    /// As above, remembering Efficiency mode's classes in @p efficiencyPriorities rather than the process's shared one
    /// (sharedEfficiencyModePriorities()): tests, for a TaskSmack's and a helper's memory in one process (#3327).
    WindowsProcessActions(WindowsProcessFunctions functions, std::shared_ptr<EfficiencyModePriorities> efficiencyPriorities);
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
    /// SetPriorityClass() with niceToPriorityClass(@p nice). MIN_NICE is Realtime (#3429), read back
    /// afterwards: Windows quietly sets High instead for a caller without SeIncreaseBasePriorityPrivilege,
    /// and that is a permission refusal (realtimeReadBackResult()), so the elevated helper can retry it.
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;
    /// The process's I/O priority hint (#3362): NtSetInformationProcess(ProcessIoPriority) on the verified handle,
    /// as Process Explorer sets it. Only IoPriorityClass::WindowsHint, at Very low, Low, Normal or High
    /// (ioPriorityHintToSet()); Critical, the system's own, is refused before anything is opened, as is PID 4.
    /// The critical-process guard refuses a critical process, as a priority change. High needs
    /// SeIncreaseBasePriorityPrivilege, enabled first where TaskSmack is elevated; without it Windows refuses,
    /// and that is a permission refusal, so the elevated helper can retry it.
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) override;
    /// NtQueryInformationProcess(ProcessIoPriority) through the identity check, with
    /// PROCESS_QUERY_LIMITED_INFORMATION: an IoPriority of class WindowsHint (ioPriorityReadResult()).
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
    /// class it had (sharedEfficiencyModePriorities()) and returning it (priorityClassBefore, #3327); off
    /// hands power throttling back to Windows and, while the process is still at Idle, restores that class,
    /// or Normal when it isn't known, with a successNote saying so (#3281). Refused for PID 4 and for a
    /// protected or critical process, through the same identity check and guard as a priority change; access
    /// denied is permissionDenied, so the elevated helper can retry it. A failure to lower the class undoes
    /// the throttling, so a refused "on" changes nothing.
    [[nodiscard]] ProcessActionResult setEfficiencyMode(const ProcessTarget& target, bool on) override;
    /// Realtime's nice while @p target's off is the retry of a Realtime restore Windows set High for (#3590): the
    /// helper, as an administrator, is asked to set Realtime as a priority change, so it never infers it from High.
    /// Only while the process is still at that High, read now (#3606): at any other class the retry mark is cleared
    /// and nullopt answered, so the ordinary off runs and leaves a class someone changed it to since.
    [[nodiscard]] std::optional<std::int32_t> efficiencyNiceToRestore(const ProcessTarget& target) const override;
    /// The class remembered for @p target's off (#3327), left remembered.
    [[nodiscard]] std::optional<std::uint32_t> efficiencyPriorityClassToRestore(const ProcessTarget& target) const override;
    /// Remembers @p priorityClass for @p target's off (#3327), as an on here does; one of Windows' six classes only.
    void rememberEfficiencyPriorityClass(const ProcessTarget& target, std::uint32_t priorityClass) override;
    /// @p priorityClass, from an off's request, replaces the class remembered for @p target, Idle included (#3327).
    void setEfficiencyPriorityClassToRestore(const ProcessTarget& target, std::uint32_t priorityClass) override;
    /// Forgets the class remembered for @p target (#3327).
    void forgetEfficiencyPriorityClass(const ProcessTarget& target) override;
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
    /// The Wait Chain Traversal API on each of the process's threads (#3084; Windows::readWaitChains()),
    /// as TaskSmack's user, through the same identity check as every action.
    [[nodiscard]] WaitChainReadResult readWaitChains(const ProcessTarget& target, const std::stop_token& stopToken) override;
    /// One thread's Suspend (SuspendThread), Resume (ResumeThread) or End (TerminateThread) (#3069), as Process
    /// Explorer's Threads tab. Refused before anything is opened for the System process, TaskSmack's own process
    /// and refuseThreadActionsOn()'s (threadActionRefusal()); then through the process's identity check, the
    /// critical and protected guards for Suspend and End, as the process-level actions, and checks on the handle acted
    /// through that Windows says the thread is the process's (GetProcessIdOfThread) and was created at
    /// @p threadCreateTime (GetThreadTimes): a TID reused since the user confirmed, or an unknown time (0), is refused. Access
    /// denied opening the thread is permissionDenied, so the elevated helper can retry it, unless the process
    /// is protected. Windows counts suspensions: the success note says so when the count isn't the plain one.
    /// Open file location (#3426): the executable's path (QueryFullProcessImageNameW) through the same
    /// identity check as every action, then Explorer on it, selected: SHOpenFolderAndSelectItems, or
    /// explorer.exe /select when that fails (Windows::revealInExplorer()). As TaskSmack's user, never the
    /// elevated helper. A process whose path can't be read (System, a protected process) is an error
    /// saying so; the System process (PID 4) is refused before anything is opened: it has no file.
    [[nodiscard]] ProcessActionResult openFileLocation(const ProcessTarget& target) override;
    /// Run new task (#3427): the command line opened with ShellExecuteExW (Windows::runShellProgram()), so a
    /// document, a folder or a web address works too; as administrator through the runas verb's UAC prompt,
    /// unless TaskSmack is elevated already (read once, at construction), when it simply opens it, elevated.
    /// A dismissed prompt is a cancelled result. As TaskSmack's user, never the elevated helper.
    [[nodiscard]] ProcessActionResult runProgram(const RunProgramRequest& request) override;
    [[nodiscard]] ProcessActionResult
    actOnThread(const ProcessTarget& target, std::int32_t tid, std::uint64_t threadCreateTime, ThreadAction action) override;
    /// Close connection (#3358): the IPv4 TCP connection reset with SetTcpEntry(MIB_TCP_STATE_DELETE_TCB)
    /// (Windows::closeTcpConnectionOf()), as TCPView's. Refused for PID 4 before anything is opened, then through the
    /// identity check (a limited-query handle, held while the table is read again and the entry set) and the
    /// critical-process guard, as a priority change. Windows lets only an administrator set a connection's state,
    /// so a refusal is permissionDenied and the elevated helper can retry it.
    [[nodiscard]] ProcessActionResult closeTcpConnection(const ProcessTarget& target, const TcpConnectionToClose& connection) override;

    /// Also refuses thread actions on @p pid's threads (#3069): in the elevated helper, the TaskSmack it
    /// serves, whose suspended or ended thread could freeze or corrupt it. 0: none but this process's own.
    void refuseThreadActionsOn(DWORD pid) noexcept
    {
        m_ThreadActionRefusedPid = pid;
    }

  private:
    /// Helper to terminate a process with given exit code
    [[nodiscard]] ProcessActionResult terminateProcess(const ProcessTarget& target, uint32_t exitCode) const;
    /// stop() (@p suspend) or resume().
    [[nodiscard]] ProcessActionResult suspendOrResume(const ProcessTarget& target, bool suspend) const;

    WindowsProcessFunctions m_Fns;
    std::vector<std::uint32_t> m_GroupSizes; // Each processor group's maximum processor count, read at construction
    bool m_Elevated = false;                 // TaskSmack's token elevated, read at construction (#3427)
    DWORD m_ThreadActionRefusedPid = 0;      // refuseThreadActionsOn()'s
    std::shared_ptr<EfficiencyModePriorities> m_EfficiencyPriorities; // Efficiency mode's replaced classes (#3071, #3327)
};

} // namespace Platform
