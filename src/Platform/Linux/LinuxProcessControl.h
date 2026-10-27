#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "SyscallTrace.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sched.h>
#include <sys/types.h>

namespace Platform
{

/// The system calls LinuxProcessControl and LinuxProcessActions make, as a table so tests can make each
/// fail as a kernel would (#1548): an old kernel without pidfds, a process that exits between the check
/// and the call, a permission refused. realProcessSyscalls() is the real one. Each returns as the call
/// does -- a descriptor or 0 on success, -1 with errno set on failure -- except where noted.
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
    /// used; tests fake it to reach what runs once ptrace allows the attach (#807). LinuxProcessActions only.
    SyscallTrace::PtraceContext (*ptraceContext)(std::int32_t pid, const std::string& toolPath) = nullptr;
    /// waitpid(pid, status, options), as createDump() waits on gcore. Left null, the real call is used;
    /// tests fake a failure to see gcore's process group still killed and reaped (#2148, #2479).
    pid_t (*waitPid)(pid_t pid, int* status, int options) = nullptr;
    /// The state letter in /proc/<pid>/stat ('T' or 't' when stopped), '\0' when it can't be read. Left
    /// null, the real /proc is read; tests script a ptrace stop for a cancelled dump to wake (#2122, #2479).
    char (*processState)(std::int32_t pid) = nullptr;
    /// The TracerPid in /proc/<pid>/status (0 for nobody), nullopt when it can't be read (the process is
    /// gone). Left null, the real /proc is read.
    std::optional<std::int32_t> (*tracerPid)(std::int32_t pid) = nullptr;
    /// Where /proc/<pid>/exe points (readlink), or the errno that refused it (ENOENT for a kernel thread,
    /// EACCES for another user's process). Left null, the real link is read. LinuxProcessActions only (#3064).
    std::expected<std::string, int> (*executableLink)(std::int32_t pid) = nullptr;
    /// org.freedesktop.FileManager1.ShowItems for file:// URI @p uri on the session bus: true when a file
    /// manager took it. Left null, the real call is made (none without sd-bus, which is a false); tests
    /// fake it so no file manager opens. LinuxProcessActions only (#3064).
    bool (*showItemInFileManager)(const std::string& uri) = nullptr;
};

/// The real system calls.
[[nodiscard]] LinuxProcessSyscalls realProcessSyscalls() noexcept;

/// The process-control half of the Linux IProcessActions: signals, nice, I/O priority and CPU affinity,
/// and nothing that starts another program. tasksmack-helper, which runs as root, links only this, so no
/// strace, gcore or terminal launch -- nor the PATH search for them -- is in it (#2640). TaskSmack itself
/// uses LinuxProcessActions, which adds those.
///
/// Signals go through a pidfd: the action opens one for the PID, confirms the start time in
/// /proc/[pid]/stat matches the target's, and signals through the pidfd, which keeps referring to
/// the process it was opened for even if the PID is later reused. setpriority(2) has no pidfd form,
/// so it is aimed at the PID after the check, and the pidfd is asked afterwards whether the target
/// survived the call: if it did, its PID never changed hands. ioprio_set(2) and ioprio_get(2) (#803) are
/// checked the same way. Where pidfds are unavailable (kernels
/// before 5.3, or a seccomp filter that blocks pidfd_open) every action is refused rather than sent
/// to the bare PID.
///
/// actionCapabilities() reports the system call trace as Unsupported and no dump files.
class LinuxProcessControl : public IProcessActions
{
  public:
    /// Makes the real system calls.
    LinuxProcessControl();
    /// Makes its system calls through @p syscalls (tests: calls that fail on demand, #1548).
    explicit LinuxProcessControl(const LinuxProcessSyscalls& syscalls);
    ~LinuxProcessControl() override = default;

    LinuxProcessControl(const LinuxProcessControl&) = delete;
    LinuxProcessControl& operator=(const LinuxProcessControl&) = delete;
    LinuxProcessControl(LinuxProcessControl&&) = default;
    LinuxProcessControl& operator=(LinuxProcessControl&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) override;
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus) override;
    /// The online CPUs and, on a hybrid CPU, their efficiency classes, from sysfs (#1881).
    [[nodiscard]] CpuAffinityLayout affinityLayout() const override;

  protected:
    /// The system calls this makes, for LinuxProcessActions' trace and dump.
    [[nodiscard]] const LinuxProcessSyscalls& syscalls() const noexcept
    {
        return m_Syscalls;
    }

  private:
    [[nodiscard]] ProcessActionResult sendSignal(const ProcessTarget& target, int signal, std::string_view signalName) const;

    LinuxProcessSyscalls m_Syscalls;
    bool m_CanSetRealtimeIoPriority = false; // CAP_SYS_NICE or CAP_SYS_ADMIN, read at construction (#1540)
};

} // namespace Platform
