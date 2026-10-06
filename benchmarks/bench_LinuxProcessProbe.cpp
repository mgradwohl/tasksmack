// Benchmarks for Platform::LinuxProcessProbe against a synthetic /proc (Linux only)
//
// BM_ProcessProbe_Enumerate (bench_ProcessModel.cpp) times the real /proc, whose size and contents
// depend on the machine. These build a fixed /proc of thousands of processes in a temporary
// directory, so runs and machines compare like for like at a process count where the per-process
// reads dominate (#598: plan #843's Phase 3b, #1425).

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxProcessProbe.h"
#include "Platform/PlatformConfig.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace
{

/// A synthetic /proc (plus the powercap, cgroup and CPU sysfs roots the probe reads) of `count`
/// processes, each with the files enumerate() reads: stat, statm, a realistic ~1.4 KiB status, a
/// command line, io, a cgroup (one of 200 app scopes), and an fd directory of eight links, two of them sockets. Removed again
/// when the benchmark binary exits.
class SyntheticProc
{
  public:
    explicit SyntheticProc(int count)
        : m_Root(std::filesystem::temp_directory_path() / std::format("tasksmack-bench-proc-{}-{}", ::getpid(), count))
    {
        std::filesystem::remove_all(m_Root);
        write(proc() / "stat", "cpu  100000 0 50000 800000 0 0 0 0 0 0\nbtime 1700000000\n");
        write(proc() / "self" / "io", "read_bytes: 0\nwrite_bytes: 0\n");
        write(cpuSysfs() / "online", "0-15\n");
        for (int scope = 0; scope < CGROUP_COUNT; ++scope)
        {
            write(cgroup() / cgroupPath(scope) / "cgroup.events", "populated 1\nfrozen 0\n");
        }
        std::filesystem::create_directories(powercap());

        std::uint64_t nextSocketInode = 100'000;
        for (int pid = FIRST_PID; pid < FIRST_PID + count; ++pid)
        {
            const auto dir = proc() / std::to_string(pid);
            const std::string name = std::format("worker-{}", pid % 97);
            write(dir / "stat",
                  std::format("{} ({}) S 1 {} {} 0 -1 4194304 1200 0 3 0 {} {} 0 0 20 0 4 0 {} 123456789 2500 "
                              "18446744073709551615 1 1 0 0 0 0 0 4096 17920 0 0 0 17 3 0 0 0 0 0\n",
                              pid,
                              name,
                              pid,
                              pid,
                              pid % 1000,
                              pid % 500,
                              1000 + pid));
            write(dir / "statm", "30141 2500 1200 300 0 4000 0\n");
            write(dir / "status", status(pid, name));
            write(dir / "cmdline", cmdline(pid, name));
            write(dir / "io",
                  std::format("rchar: {}\nwchar: {}\nsyscr: 100\nsyscw: 50\nread_bytes: {}\nwrite_bytes: {}\n"
                              "cancelled_write_bytes: 0\n",
                              pid * 10,
                              pid * 5,
                              pid * 4096,
                              pid * 8192));
            write(dir / "cgroup", std::format("0::/{}\n", cgroupPath(pid % CGROUP_COUNT)));

            const auto fdDir = dir / "fd";
            std::filesystem::create_directories(fdDir);
            std::filesystem::create_symlink("/dev/null", fdDir / "0");
            std::filesystem::create_symlink("/dev/pts/0", fdDir / "1");
            std::filesystem::create_symlink("/dev/pts/0", fdDir / "2");
            std::filesystem::create_symlink(std::format("/var/log/app/{}.log", pid), fdDir / "3");
            std::filesystem::create_symlink("anon_inode:[eventpoll]", fdDir / "4");
            std::filesystem::create_symlink(std::format("pipe:[{}]", nextSocketInode++), fdDir / "5");
            std::filesystem::create_symlink(std::format("socket:[{}]", nextSocketInode++), fdDir / "6");
            std::filesystem::create_symlink(std::format("socket:[{}]", nextSocketInode++), fdDir / "7");
        }
    }

    ~SyntheticProc()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_Root, ignored);
    }

    SyntheticProc(const SyntheticProc&) = delete;
    SyntheticProc& operator=(const SyntheticProc&) = delete;
    SyntheticProc(SyntheticProc&&) = delete;
    SyntheticProc& operator=(SyntheticProc&&) = delete;

    [[nodiscard]] std::filesystem::path proc() const
    {
        return m_Root / "proc";
    }
    [[nodiscard]] std::filesystem::path powercap() const
    {
        return m_Root / "powercap";
    }
    [[nodiscard]] std::filesystem::path cgroup() const
    {
        return m_Root / "cgroup";
    }
    [[nodiscard]] std::filesystem::path cpuSysfs() const
    {
        return m_Root / "cpu";
    }

    [[nodiscard]] std::unique_ptr<Platform::LinuxProcessProbe> makeProbe() const
    {
        return std::make_unique<Platform::LinuxProcessProbe>(proc(), powercap(), cgroup(), cpuSysfs());
    }

  private:
    static constexpr int FIRST_PID = 1000;
    // Processes share cgroups, as on a desktop's systemd user session: about 25 per app scope.
    static constexpr int CGROUP_COUNT = 200;
    std::filesystem::path m_Root;

    static void write(const std::filesystem::path& path, std::string_view content)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }

    [[nodiscard]] static std::string cgroupPath(int scope)
    {
        return std::format("user.slice/user-1000.slice/user@1000.service/app.slice/app-{}.scope", scope);
    }

    [[nodiscard]] static std::string status(int pid, std::string_view name)
    {
        // The fields and order of a real 6.x kernel's status file, about 1.4 KiB.
        return std::format("Name:\t{0}\nUmask:\t0022\nState:\tS (sleeping)\nTgid:\t{1}\nNgid:\t0\nPid:\t{1}\nPPid:\t1\n"
                           "TracerPid:\t0\nUid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\nFDSize:\t64\n"
                           "Groups:\t4 24 27 30 46 100 118 1000 \nNStgid:\t{1}\nNSpid:\t{1}\nNSpgid:\t{1}\nNSsid:\t{1}\n"
                           "Kthread:\t0\nVmPeak:\t  130000 kB\nVmSize:\t  120564 kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\n"
                           "VmHWM:\t   12000 kB\nVmRSS:\t   10000 kB\nRssAnon:\t    4000 kB\nRssFile:\t    5000 kB\n"
                           "RssShmem:\t    1000 kB\nVmData:\t   16000 kB\nVmStk:\t     132 kB\nVmExe:\t    1200 kB\n"
                           "VmLib:\t    9000 kB\nVmPTE:\t     120 kB\nVmSwap:\t       0 kB\nHugetlbPages:\t       0 kB\n"
                           "CoreDumping:\t0\nTHP_enabled:\t1\nuntag_mask:\t0xffffffffffffffff\nThreads:\t4\n"
                           "SigQ:\t0/63446\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n"
                           "SigBlk:\t0000000000000000\nSigIgn:\t0000000000001000\nSigCgt:\t0000000180004a03\n"
                           "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n"
                           "CapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t0\n"
                           "Seccomp_filters:\t0\nSpeculation_Store_Bypass:\tthread vulnerable\n"
                           "SpeculationIndirectBranch:\tconditional enabled\nCpus_allowed:\tffff\n"
                           "Cpus_allowed_list:\t0-15\nMems_allowed:\t00000000,00000001\nMems_allowed_list:\t0\n"
                           "voluntary_ctxt_switches:\t{2}\nnonvoluntary_ctxt_switches:\t{3}\n",
                           name,
                           pid,
                           pid * 3,
                           pid % 17);
    }

    [[nodiscard]] static std::string cmdline(int pid, std::string_view name)
    {
        // NUL-separated arguments, NUL-terminated, as the kernel writes them.
        std::string text;
        for (const std::string& arg : {std::format("/usr/lib/app/{}", name), std::string("--config"), std::format("/etc/app/{}.conf", pid)})
        {
            text += arg;
            text.push_back('\0');
        }
        return text;
    }
};

/// One fixture per process count, built on first use and shared by every benchmark (and repetition).
const SyntheticProc& syntheticProc(int count)
{
    static std::map<int, std::unique_ptr<SyntheticProc>> fixtures;
    auto& fixture = fixtures[count];
    if (!fixture)
    {
        fixture = std::make_unique<SyntheticProc>(count);
    }
    return *fixture;
}

// Steady-state enumerate() of a synthetic /proc: one warm-up pass first, so the per-process caches
// (#1425's command lines) are as they are between samples in the app. Network attribution is off,
// so no pass is also the periodic socket inode-to-PID map rebuild.
static void BM_ProcessProbe_EnumerateSynthetic(benchmark::State& state)
{
    const auto& fixture = syntheticProc(static_cast<int>(state.range(0)));
    const auto probe = fixture.makeProbe();
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    probe->setSocketStatsForTesting(nullptr);
#endif
    const auto warmUp = probe->enumerate();
    if (warmUp.size() != static_cast<std::size_t>(state.range(0)))
    {
        state.SkipWithError("synthetic /proc enumerated short");
        return;
    }

    for (auto _ : state)
    {
        auto processes = probe->enumerate();
        benchmark::DoNotOptimize(processes.data());
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(warmUp.size()));
}
BENCHMARK(BM_ProcessProbe_EnumerateSynthetic)->Arg(5000)->Unit(benchmark::kMillisecond);

} // namespace

#endif // __linux__ && unistd.h
