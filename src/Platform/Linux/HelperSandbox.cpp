#include "HelperSandbox.h"

#include "Platform/Linux/HelperSandboxMode.h"

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - kernel and syscall headers: include-cleaner lacks mappings for
// SYS_*, BPF_*, SECCOMP_*, AUDIT_ARCH_*, O_*, PROT_* and sock_filter
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Platform::HelperSandbox
{

namespace
{

#if defined(__x86_64__)
constexpr std::uint32_t NATIVE_ARCH = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t NATIVE_ARCH = AUDIT_ARCH_AARCH64;
#endif

#if defined(__x86_64__) || defined(__aarch64__)

constexpr std::uint32_t ALLOW = SECCOMP_RET_ALLOW;
constexpr std::uint32_t KILL = SECCOMP_RET_KILL_PROCESS;

[[nodiscard]] constexpr std::uint32_t fail(int error)
{
    return SECCOMP_RET_ERRNO | (static_cast<std::uint32_t>(error) & SECCOMP_RET_DATA);
}

constexpr std::uint32_t NR_OFFSET = offsetof(seccomp_data, nr);
constexpr std::uint32_t ARCH_OFFSET = offsetof(seccomp_data, arch);

/// The low 32 bits of argument @p index (both architectures are little-endian): an int or a flags word,
/// which the kernel reads as 32 bits, so the high half needn't be checked unless it's a pointer.
[[nodiscard]] constexpr std::uint32_t argLow(std::size_t index)
{
    return static_cast<std::uint32_t>(offsetof(seccomp_data, args) + (index * sizeof(std::uint64_t)));
}

[[nodiscard]] constexpr std::uint32_t argHigh(std::size_t index)
{
    return argLow(index) + 4;
}

/// TCGETS2 (_IOR('T', 0x2A, struct termios2)), the same on x86-64 and AArch64 (asm-generic/ioctls.h);
/// written out because its macro needs struct termios2, which <termios.h> can't be included beside.
constexpr std::uint32_t TCGETS2_REQUEST = 0x802C542A;
constexpr std::uint32_t TCGETS_REQUEST = 0x5401;

/// The open flags that write or create: none of them may reach openat(). O_TMPFILE without its
/// O_DIRECTORY bit (glibc's O_TMPFILE and __O_TMPFILE both include it), which would refuse opendir().
constexpr std::uint32_t WRITE_OPEN_FLAGS =
    O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND | static_cast<std::uint32_t>(O_TMPFILE & ~O_DIRECTORY);

[[nodiscard]] constexpr sock_filter statement(std::uint16_t code, std::uint32_t k)
{
    return sock_filter{.code = code, .jt = 0, .jf = 0, .k = k};
}

[[nodiscard]] constexpr sock_filter jump(std::uint16_t code, std::uint32_t k, std::uint8_t jt, std::uint8_t jf)
{
    return sock_filter{.code = code, .jt = jt, .jf = jf, .k = k};
}

[[nodiscard]] constexpr sock_filter load(std::uint32_t offset)
{
    return statement(BPF_LD | BPF_W | BPF_ABS, offset);
}

[[nodiscard]] constexpr sock_filter ret(std::uint32_t action)
{
    return statement(BPF_RET | BPF_K, action);
}

/// Builds the program: each rule tests the system call number (kept in the accumulator) and returns, or
/// runs a self-contained block that ends in returns, so no jump is longer than a block (BPF's are 8-bit).
class Program
{
  public:
    void returnFor(long call, std::uint32_t action)
    {
        m_Code.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(call), 0, 1));
        m_Code.push_back(ret(action));
    }

    /// For @p call, runs @p block (which must end every path in a return); others skip it.
    void blockFor(long call, const std::vector<sock_filter>& block)
    {
        m_Code.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(call), 0, static_cast<std::uint8_t>(block.size())));
        m_Code.insert(m_Code.end(), block.begin(), block.end());
    }

    void push(sock_filter instruction)
    {
        m_Code.push_back(instruction);
    }

    [[nodiscard]] std::vector<sock_filter> take()
    {
        return std::move(m_Code);
    }

  private:
    std::vector<sock_filter> m_Code;
};

/// Allowed when argument @p index has none of @p bits; else fails with @p error.
[[nodiscard]] std::vector<sock_filter> withoutBits(std::size_t index, std::uint32_t bits, int error)
{
    return {load(argLow(index)), jump(BPF_JMP | BPF_JSET | BPF_K, bits, 0, 1), ret(fail(error)), ret(ALLOW)};
}

/// Allowed on the broker socket (fd 0, argument 0) only.
[[nodiscard]] std::vector<sock_filter> onBrokerSocketOnly()
{
    return {load(argLow(0)), jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1), ret(ALLOW), ret(fail(EPERM))};
}

/// System calls allowed with any arguments: the runtime, read-only file access, the event loop and the
/// helper's own job (signals, priorities, affinity).
constexpr std::array ALLOWED{
    // Runtime
    SYS_read,
    SYS_pread64,
    SYS_readv,
    SYS_preadv,
    SYS_write,
    SYS_writev,
    SYS_close,
#ifdef SYS_close_range
    SYS_close_range,
#endif
    SYS_dup,
    SYS_dup3,
    SYS_fcntl,
    SYS_lseek,
    SYS_brk,
    SYS_munmap,
    SYS_mremap,
    SYS_madvise,
    SYS_futex,
    SYS_getrandom,
    SYS_exit,
    SYS_exit_group,
    SYS_restart_syscall,
    SYS_rt_sigreturn,
    SYS_rt_sigprocmask,
    SYS_rt_sigaction,
    SYS_sigaltstack,
    SYS_clock_gettime,
    SYS_clock_getres,
    SYS_clock_nanosleep,
    SYS_nanosleep,
    SYS_gettimeofday,
    SYS_gettid,
    SYS_getpid,
    SYS_getppid,
    SYS_getuid,
    SYS_geteuid,
    SYS_getgid,
    SYS_getegid,
    SYS_getresuid,
    SYS_getresgid,
    SYS_getgroups,
    SYS_capget,
    SYS_uname,
    SYS_sysinfo,
    SYS_getrusage,
    SYS_getrlimit,
    SYS_sched_yield,
    SYS_getpgid,
    SYS_getsid,
    // Files, reading: whatever the helper can open is read-only (openat below)
    SYS_fstat,
    SYS_newfstatat,
    SYS_statx,
    SYS_fstatfs,
    SYS_statfs,
    SYS_readlinkat,
    SYS_getdents64,
    SYS_faccessat,
#ifdef SYS_faccessat2
    SYS_faccessat2,
#endif
    SYS_fadvise64,
    SYS_getcwd,
    // The serve loop
    SYS_ppoll,
    SYS_pselect6,
    SYS_epoll_create1,
    SYS_epoll_ctl,
    SYS_epoll_pwait,
#ifdef SYS_epoll_pwait2
    SYS_epoll_pwait2,
#endif
    SYS_timerfd_create,
    SYS_timerfd_settime,
    SYS_timerfd_gettime,
// The helper's job (BrokerProtocol's actions); kill, tkill and tgkill only to itself (below)
#ifdef SYS_pidfd_open
    SYS_pidfd_open,
#endif
#ifdef SYS_pidfd_send_signal
    SYS_pidfd_send_signal,
#endif
    SYS_setpriority,
    SYS_getpriority,
    SYS_ioprio_set,
    SYS_ioprio_get,
    SYS_sched_setaffinity,
    SYS_sched_getaffinity,
    SYS_sched_getparam,
    SYS_sched_getscheduler,
#if defined(__x86_64__)
    // x86-64's older forms of the above (AArch64 has only the *at and p* ones)
    SYS_poll,
    SYS_select,
    SYS_epoll_wait,
    SYS_stat,
    SYS_lstat,
    SYS_access,
    SYS_readlink,
    SYS_getdents,
    SYS_dup2,
    SYS_time,
    SYS_getpgrp,
#endif
};

/// System calls refused by name with EPERM: what code run in the helper by a bug would want.
constexpr std::array REFUSED{
    // Escapes through the kept capabilities: file handles (CAP_DAC_READ_SEARCH), another process's
    // memory or fds (CAP_SYS_PTRACE)
    SYS_open_by_handle_at,
    SYS_name_to_handle_at,
    SYS_ptrace,
    SYS_process_vm_readv,
    SYS_process_vm_writev,
#ifdef SYS_pidfd_getfd
    SYS_pidfd_getfd,
#endif
    // New code or processes
    SYS_execve,
    SYS_execveat,
    SYS_clone,
#ifdef SYS_clone3
    SYS_clone3,
#endif
    SYS_memfd_create,
    // Sockets: the broker's is inherited. sendmsg and recvmsg aren't used (send and recv are sendto
    // and recvfrom), and would carry fds (SCM_RIGHTS).
    SYS_sendmsg,
    SYS_recvmsg,
    SYS_sendmmsg,
    SYS_recvmmsg,
    SYS_socket,
    SYS_socketpair,
    SYS_connect,
    SYS_bind,
    SYS_listen,
    SYS_accept,
    SYS_accept4,
    // Changing files
    SYS_unlinkat,
    SYS_renameat2,
    SYS_mkdirat,
    SYS_linkat,
    SYS_symlinkat,
    SYS_fchmod,
    SYS_fchmodat,
    SYS_fchown,
    SYS_fchownat,
    SYS_truncate,
    SYS_ftruncate,
    SYS_fallocate,
    SYS_mknodat,
    SYS_utimensat,
    SYS_setxattr,
    SYS_lsetxattr,
    SYS_fsetxattr,
    SYS_removexattr,
    SYS_lremovexattr,
    SYS_fremovexattr,
#ifdef SYS_renameat
    SYS_renameat, // AArch64 has it too
#endif
#ifdef SYS_fchmodat2
    SYS_fchmodat2,
#endif
    // Code by another route than mmap: READ_IMPLIES_EXEC adds PROT_EXEC to every readable mapping, and
    // SysV shared memory maps with SHM_EXEC. SysV and POSIX message queues are channels other users
    // could open, past the openat check.
    SYS_personality,
    SYS_shmget,
    SYS_shmat,
    SYS_shmctl,
    SYS_shmdt,
    SYS_msgget,
    SYS_msgsnd,
    SYS_msgrcv,
    SYS_msgctl,
    SYS_semget,
    SYS_semop,
    SYS_semtimedop,
    SYS_semctl,
    SYS_mq_open,
    SYS_mq_unlink,
    SYS_mq_timedsend,
    SYS_mq_timedreceive,
    SYS_mq_notify,
    SYS_mq_getsetattr,
    // CAP_SYS_NICE beyond the helper's job (nice, I/O priority, affinity): real-time scheduling, which
    // can starve every CPU, and other processes' memory placement
    SYS_sched_setscheduler,
    SYS_sched_setattr,
    SYS_migrate_pages,
    SYS_move_pages,
    SYS_mbind,
    SYS_set_mempolicy,
#ifdef SYS_process_madvise
    SYS_process_madvise,
#endif
    // Mounts and namespaces
    SYS_mount,
    SYS_umount2,
    SYS_pivot_root,
    SYS_chroot,
    SYS_setns,
    SYS_unshare,
#ifdef SYS_fsopen
    SYS_fsopen,
#endif
#ifdef SYS_fsmount
    SYS_fsmount,
#endif
#ifdef SYS_fspick
    SYS_fspick,
#endif
#ifdef SYS_move_mount
    SYS_move_mount,
#endif
#ifdef SYS_open_tree
    SYS_open_tree,
#endif
#ifdef SYS_mount_setattr
    SYS_mount_setattr,
#endif
    // The kernel's larger attack surfaces
    SYS_bpf,
    SYS_perf_event_open,
    SYS_io_uring_setup,
    SYS_io_uring_enter,
    SYS_io_uring_register,
    SYS_userfaultfd,
    SYS_kexec_load,
    SYS_kexec_file_load,
    SYS_init_module,
    SYS_finit_module,
    SYS_delete_module,
    SYS_keyctl,
    SYS_add_key,
    SYS_request_key,
    // Its identity, capabilities and this filter
    SYS_prctl,
    SYS_seccomp,
    SYS_setuid,
    SYS_setgid,
    SYS_setreuid,
    SYS_setregid,
    SYS_setresuid,
    SYS_setresgid,
    SYS_setgroups,
    SYS_setfsuid,
    SYS_setfsgid,
    SYS_capset,
#if defined(__x86_64__)
    SYS_fork,
    SYS_vfork,
    SYS_creat,
    SYS_unlink,
    SYS_rename,
    SYS_mkdir,
    SYS_rmdir,
    SYS_link,
    SYS_symlink,
    SYS_chmod,
    SYS_chown,
    SYS_lchown,
    SYS_mknod,
    SYS_uselib,
    SYS_iopl,
    SYS_ioperm,
    SYS_modify_ldt,
    SYS_utime,
    SYS_utimes,
    SYS_futimesat,
#endif
};

#endif // x86-64 or AArch64

} // namespace

bool supported() noexcept
{
#if defined(__x86_64__) || defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

std::vector<sock_filter> program([[maybe_unused]] Mode mode, [[maybe_unused]] pid_t self)
{
#if defined(__x86_64__) || defined(__aarch64__)
    Program code;
    // The architecture first: a system call number means nothing under another ABI.
    code.push(load(ARCH_OFFSET));
    code.push(jump(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_ARCH, 1, 0));
    code.push(ret(KILL));
    code.push(load(NR_OFFSET));
#if defined(__x86_64__)
    // x32's calls share AUDIT_ARCH_X86_64, marked by this bit.
    constexpr std::uint32_t X32_SYSCALL_BIT = 0x40000000;
    code.push(jump(BPF_JMP | BPF_JGE | BPF_K, X32_SYSCALL_BIT, 0, 1));
    code.push(ret(KILL));
#endif
    for (const long call : ALLOWED)
    {
        code.returnFor(call, ALLOW);
    }
    // Opens: read-only. A write or create fails as permission denied would.
    code.blockFor(SYS_openat, withoutBits(2, WRITE_OPEN_FLAGS, EACCES));
#if defined(__x86_64__)
    code.blockFor(SYS_open, withoutBits(1, WRITE_OPEN_FLAGS, EACCES));
#endif
    // Mappings: never executable, so no code is loaded; a late dlopen() fails rather than kill the helper.
    code.blockFor(SYS_mmap, withoutBits(2, PROT_EXEC, EPERM));
    code.blockFor(SYS_mprotect, withoutBits(2, PROT_EXEC, EPERM));
    code.blockFor(SYS_pkey_mprotect, withoutBits(2, PROT_EXEC, EPERM));
    // The broker socket only.
    for (const long call : {SYS_sendto, SYS_recvfrom})
    {
        code.blockFor(call, onBrokerSocketOnly());
    }
    // ioctl: isatty()'s terminal query only (spdlog on fd 1).
    code.blockFor(SYS_ioctl,
                  {load(argLow(1)),
                   jump(BPF_JMP | BPF_JEQ | BPF_K, TCGETS_REQUEST, 2, 0),
                   jump(BPF_JMP | BPF_JEQ | BPF_K, TCGETS2_REQUEST, 1, 0),
                   ret(fail(ENOTTY)),
                   ret(ALLOW)});
    // prlimit64: reading its own limits only (pid 0, no new limit).
    code.blockFor(SYS_prlimit64,
                  {load(argLow(0)),
                   jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 5),
                   load(argLow(2)),
                   jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3),
                   load(argHigh(2)),
                   jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
                   ret(ALLOW),
                   ret(fail(EPERM))});
    // Signals by number only to itself (abort(), raise()): another process is signalled through a
    // pidfd, which the user-space checks guard; kill(-1) with CAP_KILL would reach every process.
    for (const long call : {SYS_kill, SYS_tkill, SYS_tgkill})
    {
        code.blockFor(
            call, {load(argLow(0)), jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(self), 0, 1), ret(ALLOW), ret(fail(EPERM))});
    }
    // openat2's flags are in a struct the filter can't read: say it doesn't exist, and glibc uses openat.
#ifdef SYS_openat2
    code.returnFor(SYS_openat2, fail(ENOSYS));
#endif
    for (const long call : REFUSED)
    {
        code.returnFor(call, fail(EPERM));
    }
    code.push(ret(mode == Mode::Enforce ? KILL : SECCOMP_RET_LOG));
    return code.take();
#else
    return {};
#endif
}

void preloadLibraries() noexcept
{
    // libsystemd's sonames for them (src/basic/compress.c's dlopen_zstd(), dlopen_lzma(), dlopen_lz4()).
    for (const char* library : {"libzstd.so.1", "liblzma.so.5", "liblz4.so.1"})
    {
        static_cast<void>(::dlopen(library, RTLD_NOW | RTLD_NODELETE)); // kept for the process's life
    }
}

bool installFilter(Mode mode) noexcept
{
    try
    {
        std::vector<sock_filter> code = program(mode, ::getpid());
        if (code.empty() || code.size() > USHRT_MAX)
        {
            return false;
        }
        const sock_fprog fprog{.len = static_cast<unsigned short>(code.size()), .filter = code.data()};
        // FLAG_LOG: the named refusals (RET_ERRNO) are logged too, so the log-only stage sees a refusal
        // the helper needed, not only the calls it doesn't list.
        return ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_LOG, &fprog) == 0;
    }
    catch (...)
    {
        return false; // the program's allocation
    }
}

} // namespace Platform::HelperSandbox

// NOLINTEND(misc-include-cleaner)
