#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "SyscallTrace.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sched.h>
#include <sys/types.h>

namespace Platform
{

/// The system calls LinuxProcessActions makes, as a table so tests can make each fail as a kernel would
/// (#1548): an old kernel without pidfds, a process that exits between the check and the call, a
/// permission refused. realProcessSyscalls() is the real one. Each returns as the call does -- a
/// descriptor or 0 on success, -1 with errno set on failure -- except where noted.
struct LinuxProcessSyscalls
{
    int (*pidfdOpen)(pid_t pid) = nullptr;                   ///< pidfd_open(pid, 0); ENOSYS without pidfd support
    int (*pidfdSendSignal)(int pidfd, int signal) = nullptr; ///< pidfd_send_signal(pidfd, signal, nullptr, 0)
    /// poll() on the pidfd with no timeout: 1 when it is readable (its process exited), 0 when not.
    int (*pollPidfd)(int pidfd) = nullptr;
    int (*setPriority)(id_t tid, int nice) = nullptr; ///< setpriority(PRIO_PROCESS, tid, nice)
    int (*ioprioSet)(id_t tid, int ioprio) = nullptr; ///< ioprio_set(IOPRIO_WHO_PROCESS, tid, ioprio)
    int (*ioprioGet)(pid_t pid) = nullptr;            ///< ioprio_get(IOPRIO_WHO_PROCESS, pid): the value
    /// sched_setaffinity(tid, setSize, set): the set is a CPU_ALLOC()ed cpu_set_t of setSize bytes (#1887).
    int (*setAffinity)(id_t tid, std::size_t setSize, const cpu_set_t* set) = nullptr;
    /// The thread IDs listed under /proc/<pid>/task, or why they couldn't be (none listed: ESRCH). Left
    /// null, the real listing is used: most tests fake only the calls above.
    std::expected<std::vector<id_t>, std::error_code> (*listThreads)(pid_t pid) = nullptr;
    /// Whether thread @p tid is still listed under /proc/<pid>/task. Left null, the real check is used.
    bool (*isThreadOf)(pid_t pid, id_t tid) = nullptr;
    /// What ptrace(2) would check if @p toolPath (strace, gcore) attached to @p pid: Yama's scope, the
    /// target's credentials, and whether the tool holds CAP_SYS_PTRACE. Left null, the real check is
    /// used; tests fake it to reach what runs once ptrace allows the attach (#807).
    SyscallTrace::PtraceContext (*ptraceContext)(std::int32_t pid, const std::string& toolPath) = nullptr;
};

/// The real system calls.
[[nodiscard]] LinuxProcessSyscalls realProcessSyscalls() noexcept;

/// Linux implementation of IProcessActions.
/// Signals go through a pidfd: the action opens one for the PID, confirms the start time in
/// /proc/[pid]/stat matches the target's, and signals through the pidfd, which keeps referring to
/// the process it was opened for even if the PID is later reused. setpriority(2) has no pidfd form,
/// so it is aimed at the PID after the check, and the pidfd is asked afterwards whether the target
/// survived the call: if it did, its PID never changed hands. ioprio_set(2) and ioprio_get(2) (#803) are
/// checked the same way. Where pidfds are unavailable (kernels
/// before 5.3, or a seccomp filter that blocks pidfd_open) every action is refused rather than sent
/// to the bare PID.
///
/// launchSyscallTrace() opens a terminal running `strace -p` (#182). strace and the terminal are looked
/// up once, at construction (discoverSyscallTraceTools()), and actionCapabilities() reports what was
/// found; installing either later takes effect on the next start.
///
/// createDump() writes a core file with gdb's gcore (#807), found the same way: the action is offered
/// only where gcore is installed, and refused with the reason where ptrace would refuse gcore.
class LinuxProcessActions : public IProcessActions
{
  public:
    /// Finds strace and a terminal emulator on PATH (and $TERMINAL) for launchSyscallTrace().
    LinuxProcessActions();
    /// Uses @p traceTools instead of searching (tests: a fake terminal that records its argv).
    explicit LinuxProcessActions(SyscallTrace::Tools traceTools);
    /// As above, making its system calls through @p syscalls (tests: calls that fail on demand, #1548).
    LinuxProcessActions(SyscallTrace::Tools traceTools, const LinuxProcessSyscalls& syscalls);
    ~LinuxProcessActions() override = default;

    LinuxProcessActions(const LinuxProcessActions&) = delete;
    LinuxProcessActions& operator=(const LinuxProcessActions&) = delete;
    LinuxProcessActions(LinuxProcessActions&&) = default;
    LinuxProcessActions& operator=(LinuxProcessActions&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) override;
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult launchSyscallTrace(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus) override;
    /// A core file of the process, written by gcore into @p directory (#807). Live: gcore pauses the
    /// process while it copies its memory out. Once @p stopToken is requested, gcore (and the gdb it
    /// runs) is killed and the partial file deleted.
    [[nodiscard]] ProcessDumpResult
    createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override;
    /// The online CPUs and, on a hybrid CPU, their efficiency classes, from sysfs (#1881).
    [[nodiscard]] CpuAffinityLayout affinityLayout() const override;

    /// strace and the terminal to run it in, from this process's PATH and $TERMINAL, and gcore for
    /// createDump() (#807).
    [[nodiscard]] static SyscallTrace::Tools discoverSyscallTraceTools();

  private:
    [[nodiscard]] ProcessActionResult sendSignal(const ProcessTarget& target, int signal, std::string_view signalName) const;

    SyscallTrace::Tools m_TraceTools;
    LinuxProcessSyscalls m_Syscalls;
    bool m_CanSetRealtimeIoPriority = false; // CAP_SYS_NICE or CAP_SYS_ADMIN, read at construction (#1540)
};

} // namespace Platform
