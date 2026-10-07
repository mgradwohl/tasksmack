#include "SyntheticWorkload.h"

#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Synthetic
{

namespace
{

/// The shared sine periods (seconds) every wave's two harmonics pick from: a few seconds (visible
/// movement at any refresh) up to several minutes (slow drift across a long history).
constexpr std::array<double, Workload::BAND_COUNT> BAND_PERIODS_SECONDS = {2.9, 6.1, 11.3, 23.0, 47.0, 97.0, 199.0, 409.0};

[[nodiscard]] double bandOmega(std::size_t band) noexcept
{
    return 2.0 * std::numbers::pi / BAND_PERIODS_SECONDS[band]; // band < BAND_COUNT
}

/// A long-lived process starts no later than this, so it exists throughout any history window.
constexpr double MAX_LONG_LIVED_START_SECONDS = Workload::UPTIME_AT_EPOCH_SECONDS / 2.0;
/// Churning slots' PIDs: a block of CHURN_PID_BLOCK per slot above every long-lived PID.
constexpr std::int32_t CHURN_PID_BASE = 1'000'000;
constexpr std::int32_t CHURN_PID_BLOCK = 1000;
/// The most a wave's two harmonics add up to: a rate stays within base * (1 +- MAX_SWING).
constexpr double MAX_SWING = 0.9;
/// The most CPU all processes' base rates may add up to, as a fraction of the cores: a machine with
/// more busy processes than its cores can carry is scaled down to it.
constexpr double MAX_PROCESS_CPU_SHARE = 0.42;
/// How unevenly process work is spread over the cores: each core's share is within this fraction of
/// an even split before the shares are normalised to sum to 1.
constexpr double MAX_CORE_SHARE_SKEW = 0.05;
/// A core's overhead and iowait base rates (fractions of the core) are at most this.
constexpr double MAX_CORE_OVERHEAD_BASE = 0.02;
// A core never needs more than it has: at its peak, its share of all process work plus its overhead
// and iowait stays under one core, so idle time never goes backwards.
static_assert((MAX_PROCESS_CPU_SHARE * (1.0 + MAX_SWING) * (1.0 + MAX_CORE_SHARE_SKEW) / (1.0 - MAX_CORE_SHARE_SKEW)) +
                      (2.0 * MAX_CORE_OVERHEAD_BASE * (1.0 + MAX_SWING)) <
                  1.0,
              "synthetic cores would exceed their capacity");
/// An idle process's base CPU (cores): most processes sit here.
constexpr double IDLE_CPU_MIN = 0.00005;
constexpr double IDLE_CPU_MAX = 0.003;
/// The most one process's base rate may be after scaling (cores); its peak is under twice that.
constexpr double MAX_PROCESS_CPU_BASE = 1.5;
constexpr double MIB = 1024.0 * 1024.0;
constexpr double GIB = 1024.0 * MIB;
constexpr double TICKS = static_cast<double>(Workload::TICKS_PER_SECOND);

/// SplitMix64: a tiny, well-mixed generator with a fixed definition, so a seed produces the same
/// machine with every standard library (std::uniform_*_distribution's output is implementation-defined).
class SplitMix64
{
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept : m_State(seed)
    {}

    [[nodiscard]] std::uint64_t next() noexcept
    {
        m_State += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = m_State;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    /// Uniform in [0, 1), from the top 53 bits.
    [[nodiscard]] double unit() noexcept
    {
        return static_cast<double>(next() >> 11U) * 0x1.0p-53;
    }

    [[nodiscard]] double uniform(double lo, double hi) noexcept
    {
        return lo + ((hi - lo) * unit());
    }

    /// Skewed towards @p lo: most draws are small, a few large (a heavy tail).
    [[nodiscard]] double skewed(double lo, double hi, double power) noexcept
    {
        return lo + ((hi - lo) * std::pow(unit(), power));
    }

    [[nodiscard]] std::size_t below(std::size_t n) noexcept
    {
        return (n == 0) ? 0 : static_cast<std::size_t>(next() % n);
    }

    [[nodiscard]] std::int32_t between(std::int32_t lo, std::int32_t hi) noexcept
    {
        return lo + static_cast<std::int32_t>(below(static_cast<std::size_t>(hi - lo) + 1));
    }

    [[nodiscard]] bool chance(double probability) noexcept
    {
        return unit() < probability;
    }

  private:
    std::uint64_t m_State;
};

/// A wave of @p base with two harmonics whose amplitudes sum to at most @p maxSwing (< 1), so the
/// rate stays positive.
[[nodiscard]] Workload::Wave makeWave(SplitMix64& rng, double base, double maxSwing = MAX_SWING)
{
    Workload::Wave wave;
    wave.base = base;
    const double first = rng.uniform(0.25, 0.65) * maxSwing;
    const double second = rng.uniform(0.0, 1.0) * (maxSwing - first);
    wave.harmonics[0] = {.amplitude = first, .phase = rng.uniform(0.0, 2.0 * std::numbers::pi), .band = rng.below(Workload::BAND_COUNT)};
    wave.harmonics[1] = {.amplitude = second, .phase = rng.uniform(0.0, 2.0 * std::numbers::pi), .band = rng.below(Workload::BAND_COUNT)};
    return wave;
}

/// A cumulative counter from a non-negative running total; rounding a monotonic value keeps it monotonic.
[[nodiscard]] std::uint64_t toCounter(double value) noexcept
{
    return (value <= 0.0) ? 0 : static_cast<std::uint64_t>(std::llround(value));
}

/// What kind of process a slot is: sets its resource profile.
enum class Kind : std::uint8_t
{
    Init,
    Kernel,
    Daemon,
    ServiceWorker,
    Session,
    BrowserMain,
    BrowserChild,
    EditorMain,
    EditorChild,
    LanguageServer,
    ContainerShim,
    ContainerMain,
    ContainerWorker,
    Shell,
    Tool,
    BuildDriver,
    BuildWorker,
};

struct Profile
{
    double activeChance = 0.0; // chance the process is busy; the rest idle at IDLE_CPU_MIN..IDLE_CPU_MAX
    double cpuLo = 0.0;        // a busy process's base CPU (cores)
    double cpuHi = 0.0;
    double rssMibLo = 0.0;
    double rssMibHi = 0.0;
    std::int32_t threadsLo = 1;
    std::int32_t threadsHi = 1;
    std::int32_t handlesLo = 0;
    std::int32_t handlesHi = 0;
    double ioHi = 0.0; // read base bytes/s upper bound (write is about half)
    double netChance = 0.0;
    double netHi = 0.0; // receive base bytes/s upper bound (send is about a third)
    double faultsHi = 0.0;
    double lifetimeLo = 0.0; // seconds; 0 = long-lived
    double lifetimeHi = 0.0;
    std::int32_t nice = 0;
};

/// Resource profiles by kind: activeChance, busy CPU lo/hi (cores), RSS lo/hi (MiB), threads lo/hi,
/// handles lo/hi, read bytes/s, network chance, receive bytes/s, page faults/s, lifetime lo/hi (s), nice.
[[nodiscard]] Profile profileFor(Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::Init:
        return Profile{
            .rssMibLo = 8, .rssMibHi = 14, .threadsLo = 1, .threadsHi = 1, .handlesLo = 90, .handlesHi = 160, .ioHi = 2e3, .faultsHi = 5};
    case Kind::Kernel:
        return Profile{.activeChance = 0.01, .cpuLo = 0.02, .cpuHi = 0.1, .threadsLo = 1, .threadsHi = 1};
    case Kind::Daemon:
        return Profile{.activeChance = 0.05,
                       .cpuLo = 0.02,
                       .cpuHi = 0.3,
                       .rssMibLo = 4,
                       .rssMibHi = 90,
                       .threadsLo = 1,
                       .threadsHi = 16,
                       .handlesLo = 8,
                       .handlesHi = 220,
                       .ioHi = 2e5,
                       .netChance = 0.4,
                       .netHi = 2e5,
                       .faultsHi = 40};
    case Kind::ServiceWorker:
        return Profile{.activeChance = 0.08,
                       .cpuLo = 0.05,
                       .cpuHi = 0.5,
                       .rssMibLo = 12,
                       .rssMibHi = 160,
                       .threadsLo = 1,
                       .threadsHi = 4,
                       .handlesLo = 12,
                       .handlesHi = 60,
                       .ioHi = 5e5,
                       .netChance = 0.7,
                       .netHi = 4e5,
                       .faultsHi = 50};
    case Kind::Session:
        return Profile{.activeChance = 0.3,
                       .cpuLo = 0.03,
                       .cpuHi = 0.4,
                       .rssMibLo = 10,
                       .rssMibHi = 380,
                       .threadsLo = 2,
                       .threadsHi = 28,
                       .handlesLo = 20,
                       .handlesHi = 400,
                       .ioHi = 1e5,
                       .netChance = 0.1,
                       .netHi = 2e4,
                       .faultsHi = 100};
    case Kind::BrowserMain:
        return Profile{.activeChance = 1.0,
                       .cpuLo = 0.3,
                       .cpuHi = 1.0,
                       .rssMibLo = 450,
                       .rssMibHi = 900,
                       .threadsLo = 80,
                       .threadsHi = 140,
                       .handlesLo = 300,
                       .handlesHi = 600,
                       .ioHi = 2e6,
                       .netChance = 1.0,
                       .netHi = 2e6,
                       .faultsHi = 1000};
    case Kind::BrowserChild:
        return Profile{.activeChance = 0.02,
                       .cpuLo = 0.1,
                       .cpuHi = 0.9,
                       .rssMibLo = 20,
                       .rssMibHi = 180,
                       .threadsLo = 14,
                       .threadsHi = 40,
                       .handlesLo = 40,
                       .handlesHi = 140,
                       .ioHi = 1e5,
                       .netChance = 0.3,
                       .netHi = 3e5,
                       .faultsHi = 200};
    case Kind::EditorMain:
        return Profile{.activeChance = 1.0,
                       .cpuLo = 0.05,
                       .cpuHi = 0.3,
                       .rssMibLo = 200,
                       .rssMibHi = 400,
                       .threadsLo = 30,
                       .threadsHi = 60,
                       .handlesLo = 120,
                       .handlesHi = 300,
                       .ioHi = 3e5,
                       .netChance = 0.5,
                       .netHi = 5e4,
                       .faultsHi = 600};
    case Kind::EditorChild:
        return Profile{.activeChance = 0.05,
                       .cpuLo = 0.05,
                       .cpuHi = 0.4,
                       .rssMibLo = 30,
                       .rssMibHi = 200,
                       .threadsLo = 8,
                       .threadsHi = 24,
                       .handlesLo = 30,
                       .handlesHi = 90,
                       .ioHi = 2e5,
                       .netChance = 0.1,
                       .netHi = 2e4,
                       .faultsHi = 100};
    case Kind::LanguageServer:
        return Profile{.activeChance = 0.5,
                       .cpuLo = 0.2,
                       .cpuHi = 1.8,
                       .rssMibLo = 100,
                       .rssMibHi = 800,
                       .threadsLo = 8,
                       .threadsHi = 40,
                       .handlesLo = 40,
                       .handlesHi = 400,
                       .ioHi = 3e6,
                       .faultsHi = 1000};
    case Kind::ContainerShim:
        return Profile{.rssMibLo = 6, .rssMibHi = 12, .threadsLo = 9, .threadsHi = 12, .handlesLo = 12, .handlesHi = 20, .faultsHi = 2};
    case Kind::ContainerMain:
        return Profile{.activeChance = 0.15,
                       .cpuLo = 0.1,
                       .cpuHi = 1.2,
                       .rssMibLo = 40,
                       .rssMibHi = 600,
                       .threadsLo = 2,
                       .threadsHi = 120,
                       .handlesLo = 20,
                       .handlesHi = 600,
                       .ioHi = 4e6,
                       .netChance = 0.9,
                       .netHi = 4e6,
                       .faultsHi = 500};
    case Kind::ContainerWorker:
        return Profile{.activeChance = 0.03,
                       .cpuLo = 0.05,
                       .cpuHi = 0.6,
                       .rssMibLo = 20,
                       .rssMibHi = 200,
                       .threadsLo = 1,
                       .threadsHi = 8,
                       .handlesLo = 10,
                       .handlesHi = 80,
                       .ioHi = 1e6,
                       .netChance = 0.8,
                       .netHi = 1e6,
                       .faultsHi = 200};
    case Kind::Shell:
        return Profile{.rssMibLo = 4, .rssMibHi = 9, .threadsLo = 1, .threadsHi = 1, .handlesLo = 4, .handlesHi = 8, .faultsHi = 2};
    case Kind::Tool:
        return Profile{.activeChance = 0.2,
                       .cpuLo = 0.02,
                       .cpuHi = 0.3,
                       .rssMibLo = 3,
                       .rssMibHi = 120,
                       .threadsLo = 1,
                       .threadsHi = 4,
                       .handlesLo = 4,
                       .handlesHi = 30,
                       .ioHi = 2e5,
                       .netChance = 0.2,
                       .netHi = 1e5,
                       .faultsHi = 100};
    case Kind::BuildDriver:
        return Profile{
            .rssMibLo = 8, .rssMibHi = 40, .threadsLo = 1, .threadsHi = 2, .handlesLo = 8, .handlesHi = 40, .ioHi = 1e5, .faultsHi = 50};
    case Kind::BuildWorker:
        return Profile{.activeChance = 1.0,
                       .cpuLo = 0.7,
                       .cpuHi = 1.0,
                       .rssMibLo = 120,
                       .rssMibHi = 900,
                       .threadsLo = 1,
                       .threadsHi = 1,
                       .handlesLo = 6,
                       .handlesHi = 12,
                       .ioHi = 8e6,
                       .faultsHi = 5000,
                       .lifetimeLo = 4,
                       .lifetimeHi = 45,
                       .nice = 10};
    }
    return {};
}

/// A process before it gets its PID and resource waves.
struct Pending
{
    std::ptrdiff_t parent = -1; // index into the pending list; -1 = no parent (PPID 0)
    Kind kind = Kind::Tool;
    std::string name;
    std::string command;
    std::string user;
    double lifetimeLo = -1.0; // overrides the profile's lifetime when >= 0
    double lifetimeHi = -1.0;
    std::int32_t nice = 0;
    bool niceSet = false;
};

/// Builds the process list: the usual families of a busy developer workstation, in proportion to the
/// requested count, parents always before their children.
class TreeBuilder
{
  public:
    TreeBuilder(std::size_t target, std::size_t cores, std::uint64_t seed) : m_Target(target), m_Cores(cores), m_Rng(seed)
    {}

    [[nodiscard]] std::vector<Pending> build()
    {
        const std::size_t n = m_Target;
        const std::ptrdiff_t init = add(-1, Kind::Init, "systemd", "/sbin/init splash", "root");
        const std::ptrdiff_t kthreadd = add(-1, Kind::Kernel, "kthreadd", "", "root");
        addKernelThreads(kthreadd, share(n, 0.12));
        addDaemons(init, share(n, 0.08));
        const SessionRoots session = addSession(init);
        addBrowser(session.user, share(n, 0.25));
        addEditor(session.user, share(n, 0.10));
        addContainers(init, share(n, 0.20));
        addTerminals(session);
        if (m_Pending.size() > n)
        {
            m_Pending.resize(n); // parents precede children, so the tree stays whole
        }
        return std::move(m_Pending);
    }

  private:
    struct SessionRoots
    {
        std::ptrdiff_t user = -1;
        std::ptrdiff_t terminal = -1;
        std::ptrdiff_t tmux = -1;
    };

    [[nodiscard]] static std::size_t share(std::size_t n, double fraction) noexcept
    {
        return static_cast<std::size_t>(static_cast<double>(n) * fraction);
    }

    [[nodiscard]] bool full() const noexcept
    {
        return m_Pending.size() >= m_Target;
    }

    std::ptrdiff_t add(std::ptrdiff_t parent, Kind kind, std::string name, std::string command, std::string user)
    {
        m_Pending.push_back(
            Pending{.parent = parent, .kind = kind, .name = std::move(name), .command = std::move(command), .user = std::move(user)});
        return static_cast<std::ptrdiff_t>(m_Pending.size()) - 1;
    }

    void addKernelThreads(std::ptrdiff_t kthreadd, std::size_t count)
    {
        for (std::size_t k = 0; k < count && !full(); ++k)
        {
            const std::size_t core = (k / 6) % m_Cores;
            std::string name;
            std::int32_t nice = 0;
            switch (k % 6)
            {
            case 0:
                name = std::format("kworker/{}:{}", core, (k / (6 * m_Cores)) % 4);
                break;
            case 1:
                name = std::format("ksoftirqd/{}", core);
                break;
            case 2:
                name = std::format("migration/{}", core);
                break;
            case 3:
                name = std::format("kworker/{}:{}H", core, (k / (6 * m_Cores)) % 2);
                nice = -20;
                break;
            case 4:
                name = std::format("kworker/u{}:{}", 2 * m_Cores, k / 6);
                break;
            default:
                name = std::format("irq/{}-nvme0q{}", 120 + ((k / 6) % 64), core);
                break;
            }
            const std::ptrdiff_t index = add(kthreadd, Kind::Kernel, std::move(name), "", "root");
            m_Pending[static_cast<std::size_t>(index)].nice = nice;
            m_Pending[static_cast<std::size_t>(index)].niceSet = true;
        }
    }

    void addDaemons(std::ptrdiff_t init, std::size_t count)
    {
        struct DaemonEntry
        {
            const char* name;
            const char* command;
            const char* user;
        };
        static constexpr std::array DAEMONS = {
            DaemonEntry{.name = "systemd-journal", .command = "/usr/lib/systemd/systemd-journald", .user = "root"},
            DaemonEntry{.name = "systemd-udevd", .command = "/usr/lib/systemd/systemd-udevd", .user = "root"},
            DaemonEntry{.name = "systemd-resolve", .command = "/usr/lib/systemd/systemd-resolved", .user = "systemd-resolve"},
            DaemonEntry{.name = "systemd-timesyn", .command = "/usr/lib/systemd/systemd-timesyncd", .user = "systemd-timesync"},
            DaemonEntry{.name = "dbus-daemon",
                        .command = "@dbus-daemon --system --address=systemd: --nofork --systemd-activation",
                        .user = "messagebus"},
            DaemonEntry{.name = "NetworkManager", .command = "/usr/sbin/NetworkManager --no-daemon", .user = "root"},
            DaemonEntry{.name = "polkitd", .command = "/usr/lib/polkit-1/polkitd --no-debug", .user = "polkitd"},
            DaemonEntry{.name = "cron", .command = "/usr/sbin/cron -f -P", .user = "root"},
            DaemonEntry{.name = "rsyslogd", .command = "/usr/sbin/rsyslogd -n -iNONE", .user = "syslog"},
            DaemonEntry{.name = "sshd", .command = "sshd: /usr/sbin/sshd -D [listener] 0 of 10-100 startups", .user = "root"},
            DaemonEntry{.name = "avahi-daemon", .command = "avahi-daemon: running [workstation.local]", .user = "avahi"},
            DaemonEntry{.name = "cupsd", .command = "/usr/sbin/cupsd -l", .user = "root"},
            DaemonEntry{.name = "thermald", .command = "/usr/sbin/thermald --systemd --dbus-enable --adaptive", .user = "root"},
            DaemonEntry{.name = "udisksd", .command = "/usr/libexec/udisks2/udisksd", .user = "root"},
            DaemonEntry{.name = "upowerd", .command = "/usr/libexec/upowerd", .user = "root"},
            DaemonEntry{.name = "gdm3", .command = "/usr/sbin/gdm3", .user = "root"},
            DaemonEntry{.name = "snapd", .command = "/usr/lib/snapd/snapd", .user = "root"},
            DaemonEntry{
                .name = "dockerd", .command = "/usr/bin/dockerd -H fd:// --containerd=/run/containerd/containerd.sock", .user = "root"},
        };
        struct MasterEntry
        {
            const char* name;
            const char* command;
            const char* user;
            const char* workerName;
            const char* workerFormat;
        };
        static constexpr std::array MASTERS = {
            MasterEntry{.name = "postgres",
                        .command = "/usr/lib/postgresql/16/bin/postgres -D /var/lib/postgresql/16/main",
                        .user = "postgres",
                        .workerName = "postgres",
                        .workerFormat = "postgres: 16/main: app appdb 10.0.0.{} idle"},
            MasterEntry{.name = "nginx",
                        .command = "nginx: master process /usr/sbin/nginx -g daemon on; master_process on;",
                        .user = "root",
                        .workerName = "nginx",
                        .workerFormat = "nginx: worker process {}"},
            MasterEntry{.name = "php-fpm8.3",
                        .command = "php-fpm: master process (/etc/php/8.3/fpm/php-fpm.conf)",
                        .user = "root",
                        .workerName = "php-fpm8.3",
                        .workerFormat = "php-fpm: pool www {}"},
        };

        std::size_t added = 0;
        for (const auto& daemon : DAEMONS)
        {
            if (added >= count || full())
            {
                return;
            }
            add(init, Kind::Daemon, daemon.name, daemon.command, daemon.user);
            ++added;
        }
        std::array<std::ptrdiff_t, MASTERS.size()> masters{};
        for (std::size_t m = 0; m < MASTERS.size(); ++m)
        {
            if (added >= count || full())
            {
                return;
            }
            masters.at(m) = add(init, Kind::Daemon, MASTERS.at(m).name, MASTERS.at(m).command, MASTERS.at(m).user);
            ++added;
        }
        for (std::size_t w = 0; added < count && !full(); ++w, ++added)
        {
            const std::size_t m = w % MASTERS.size();
            const auto& master = MASTERS.at(m);
            add(masters.at(m),
                Kind::ServiceWorker,
                master.workerName,
                std::vformat(master.workerFormat, std::make_format_args(w)),
                (m == 0) ? "postgres" : "www-data");
        }
    }

    [[nodiscard]] SessionRoots addSession(std::ptrdiff_t init)
    {
        SessionRoots roots;
        if (full())
        {
            return roots;
        }
        roots.user = add(init, Kind::Session, "systemd", "/usr/lib/systemd/systemd --user", "synth");
        static constexpr std::array<std::pair<const char*, const char*>, 9> SESSION = {{
            {"gnome-shell", "/usr/bin/gnome-shell"},
            {"pipewire", "/usr/bin/pipewire"},
            {"wireplumber", "/usr/bin/wireplumber"},
            {"pipewire-pulse", "/usr/bin/pipewire-pulse"},
            {"Xwayland", "/usr/bin/Xwayland :0 -rootless -noreset -accessx -core -auth /run/user/1000/.mutter-Xwaylandauth"},
            {"xdg-desktop-por", "/usr/libexec/xdg-desktop-portal"},
            {"gvfsd", "/usr/libexec/gvfsd"},
            {"evolution-data-", "/usr/libexec/evolution-data-server/evolution-alarm-notify"},
            {"tracker-miner-f", "/usr/libexec/tracker-miner-fs-3"},
        }};
        for (const auto& [name, command] : SESSION)
        {
            if (full())
            {
                return roots;
            }
            const std::ptrdiff_t index = add(roots.user, Kind::Session, name, command, "synth");
            if (std::string_view(name) == "tracker-miner-f")
            {
                m_Pending[static_cast<std::size_t>(index)].nice = 19;
                m_Pending[static_cast<std::size_t>(index)].niceSet = true;
            }
        }
        if (!full())
        {
            roots.terminal = add(roots.user, Kind::Session, "gnome-terminal-", "/usr/libexec/gnome-terminal-server", "synth");
        }
        if (!full())
        {
            roots.tmux = add(roots.user, Kind::Session, "tmux: server", "tmux new-session -s work", "synth");
        }
        return roots;
    }

    void addBrowser(std::ptrdiff_t sessionUser, std::size_t count)
    {
        if (count == 0 || full() || sessionUser < 0)
        {
            return;
        }
        const std::ptrdiff_t main = add(sessionUser, Kind::BrowserMain, "firefox", "/usr/lib/firefox/firefox", "synth");
        static constexpr std::array<const char*, 5> HELPERS = {
            "Socket Process", "RDD Process", "Privileged Cont", "WebExtensions", "Utility Process"};
        std::size_t added = 1;
        for (const char* helper : HELPERS)
        {
            if (added >= count || full())
            {
                return;
            }
            add(main,
                Kind::BrowserChild,
                helper,
                std::format("/usr/lib/firefox/firefox -contentproc -parentBuildID 20261001 {}", helper),
                "synth");
            ++added;
        }
        for (std::size_t tab = 1; added < count && !full(); ++tab, ++added)
        {
            const bool isolated = (tab % 3) == 0;
            const std::ptrdiff_t index =
                add(main,
                    Kind::BrowserChild,
                    isolated ? "Isolated Web Co" : "Web Content",
                    std::format("/usr/lib/firefox/firefox -contentproc -childID {} -isForBrowser -prefsLen 31337 "
                                "-prefMapSize 244787 -parentBuildID 20261001 -greomni /usr/lib/firefox/omni.ja {} tab",
                                tab,
                                isolated ? "-fissionEnabled" : ""),
                    "synth");
            // Tabs open and close: a sixth of the content processes churn over minutes.
            if (tab % 6 == 0)
            {
                m_Pending[static_cast<std::size_t>(index)].lifetimeLo = 120.0;
                m_Pending[static_cast<std::size_t>(index)].lifetimeHi = 900.0;
            }
        }
    }

    void addEditor(std::ptrdiff_t sessionUser, std::size_t count)
    {
        if (count < 2 || full() || sessionUser < 0)
        {
            return;
        }
        const std::ptrdiff_t main = add(sessionUser, Kind::EditorMain, "code", "/usr/share/code/code --unity-launch", "synth");
        const std::ptrdiff_t zygote = add(main, Kind::EditorChild, "code", "/usr/share/code/code --type=zygote --no-sandbox", "synth");
        std::size_t added = 2;
        static constexpr std::array<std::pair<const char*, const char*>, 4> SERVERS = {{
            {"clangd", "/usr/bin/clangd --background-index --clang-tidy -j={}"},
            {"rust-analyzer", "/home/synth/.cargo/bin/rust-analyzer --threads {}"},
            {"pyright-langser", "node /home/synth/.vscode/extensions/pyright/langserver.index.js --stdio --threads {}"},
            {"gopls", "/home/synth/go/bin/gopls -remote=auto -j {}"},
        }};
        for (std::size_t window = 0; added < count && !full(); ++window)
        {
            add(zygote,
                Kind::EditorChild,
                "code",
                std::format("/usr/share/code/code --type=renderer --enable-crash-reporter --app-path=/usr/share/code/resources/app "
                            "--renderer-client-id={}",
                            window + 4),
                "synth");
            ++added;
            if (added >= count || full())
            {
                return;
            }
            const std::ptrdiff_t host = add(zygote,
                                            Kind::EditorChild,
                                            "code",
                                            std::format("/usr/share/code/code --type=utility --utility-sub-type=node.mojom.NodeService "
                                                        "--extension-host-window={}",
                                                        window),
                                            "synth");
            ++added;
            if (added >= count || full())
            {
                return;
            }
            const auto& server = SERVERS.at(window % SERVERS.size());
            add(host, Kind::LanguageServer, server.first, std::vformat(server.second, std::make_format_args(m_Cores)), "synth");
            ++added;
        }
    }

    void addContainers(std::ptrdiff_t init, std::size_t count)
    {
        if (count < 2 || full())
        {
            return;
        }
        const std::ptrdiff_t containerd = add(init, Kind::Daemon, "containerd", "/usr/bin/containerd", "root");
        std::size_t added = 1;
        struct Image
        {
            const char* main;
            const char* mainCommand;
            const char* worker;
            const char* workerFormat;
            const char* user;
        };
        static constexpr std::array IMAGES = {
            Image{.main = "gunicorn",
                  .mainCommand = "/usr/local/bin/python /usr/local/bin/gunicorn app.wsgi:application --workers 8",
                  .worker = "gunicorn",
                  .workerFormat = "/usr/local/bin/python /usr/local/bin/gunicorn app.wsgi:application --worker-id {}",
                  .user = "app"},
            Image{.main = "java",
                  .mainCommand = "java -Xmx2g -XX:+UseG1GC -jar /srv/orders-service.jar",
                  .worker = "java",
                  .workerFormat = "java -Xmx512m -jar /srv/orders-worker.jar --partition={}",
                  .user = "app"},
            Image{.main = "node",
                  .mainCommand = "node /srv/gateway/server.js --port 8080",
                  .worker = "node",
                  .workerFormat = "node /srv/gateway/worker.js --shard={}",
                  .user = "node"},
            Image{.main = "redis-server",
                  .mainCommand = "redis-server *:6379",
                  .worker = "redis-server",
                  .workerFormat = "redis-server *:{} (replica)",
                  .user = "redis"},
            Image{.main = "celery",
                  .mainCommand = "/usr/local/bin/python -m celery -A tasks worker --loglevel=INFO",
                  .worker = "celery",
                  .workerFormat = "/usr/local/bin/python -m celery -A tasks worker --hostname=worker{}@%h",
                  .user = "app"},
        };
        for (std::size_t container = 0; added < count && !full(); ++container)
        {
            const std::ptrdiff_t shim = add(containerd,
                                            Kind::ContainerShim,
                                            "containerd-shim",
                                            std::format("/usr/bin/containerd-shim-runc-v2 -namespace moby -id {:016x} -address "
                                                        "/run/containerd/containerd.sock",
                                                        m_Rng.next()),
                                            "root");
            ++added;
            if (added >= count || full())
            {
                return;
            }
            const auto& image = IMAGES.at(container % IMAGES.size());
            const std::ptrdiff_t main = add(shim, Kind::ContainerMain, image.main, image.mainCommand, image.user);
            ++added;
            const std::size_t workers = 2 + m_Rng.below(9);
            for (std::size_t w = 0; w < workers && added < count && !full(); ++w, ++added)
            {
                add(main, Kind::ContainerWorker, image.worker, std::vformat(image.workerFormat, std::make_format_args(w)), image.user);
            }
        }
    }

    /// Terminals fill whatever is left: shells running tools, and builds whose compiler processes churn.
    void addTerminals(const SessionRoots& session)
    {
        static constexpr std::array<std::pair<const char*, const char*>, 8> TOOLS = {{
            {"vim", "vim src/Domain/ProcessModel.cpp"},
            {"htop", "htop -d 10"},
            {"less", "less /var/log/syslog"},
            {"ssh", "ssh -A build@10.0.4.17"},
            {"python3", "python3 -m http.server 8000"},
            {"tail", "tail -F /var/log/nginx/access.log"},
            {"git", "git log --graph --oneline --all"},
            {"watch", "watch -n 1 nvidia-smi"},
        }};
        static constexpr std::array<std::pair<const char*, const char*>, 4> COMPILERS = {{
            {"cc1plus", "/usr/lib/gcc/x86_64-linux-gnu/14/cc1plus -quiet -I src -std=c++23 -O2 src/unit{}.cpp"},
            {"clang++", "/usr/bin/clang++ -std=c++23 -O2 -c src/module{}.cpp -o build/module.o"},
            {"ld.lld", "/usr/bin/ld.lld --threads=1 -o build/bin/target{} build/obj.o"},
            {"rustc", "rustc --crate-name crate{} --edition=2021 -C opt-level=3"},
        }};
        // Two builds at most, each with a job per core (up to 24), so their compilers are busy processes.
        constexpr std::size_t MAX_BUILDS = 2;
        std::size_t builds = 0;
        std::size_t shell = 0;
        while (!full())
        {
            std::ptrdiff_t parent = (shell % 2 == 0) ? session.terminal : session.tmux;
            if (parent < 0)
            {
                parent = (session.user >= 0) ? session.user : 0;
            }
            const std::ptrdiff_t bash = add(parent, Kind::Shell, "bash", "/bin/bash", "synth");
            ++shell;
            if (full())
            {
                return;
            }
            if (shell % 3 == 0 && builds < MAX_BUILDS)
            {
                ++builds;
                const std::ptrdiff_t make = add(bash, Kind::BuildDriver, "make", std::format("make -j{}", m_Cores), "synth");
                const std::size_t jobs = std::max<std::size_t>(2, std::min<std::size_t>(m_Cores, 24));
                for (std::size_t job = 0; job < jobs && !full(); ++job)
                {
                    const auto& compiler = COMPILERS.at(job % COMPILERS.size());
                    add(make, Kind::BuildWorker, compiler.first, std::vformat(compiler.second, std::make_format_args(job)), "synth");
                }
            }
            else
            {
                const auto& tool = TOOLS.at(m_Rng.below(TOOLS.size()));
                add(bash, Kind::Tool, tool.first, tool.second, "synth");
            }
        }
    }

    std::size_t m_Target;
    std::size_t m_Cores;
    SplitMix64 m_Rng;
    std::vector<Pending> m_Pending;
};

[[nodiscard]] std::string diskName(std::size_t index)
{
    if (index < 2)
    {
        return std::format("nvme{}n1", index);
    }
    const std::size_t letters = index - 2;
    if (letters < 26)
    {
        return std::format("sd{}", static_cast<char>('a' + letters));
    }
    return std::format("sd{}{}", static_cast<char>('a' + (((letters / 26) - 1) % 26)), static_cast<char>('a' + (letters % 26)));
}

} // namespace

// =============================================================================
// Wave
// =============================================================================

double Workload::Wave::rate(double t) const noexcept
{
    double swing = 1.0;
    for (const auto& harmonic : harmonics)
    {
        swing += harmonic.amplitude * std::sin((bandOmega(harmonic.band) * t) + harmonic.phase);
    }
    return base * swing;
}

double Workload::Wave::integral(double t) const noexcept
{
    double value = t;
    for (const auto& harmonic : harmonics)
    {
        const double omega = bandOmega(harmonic.band);
        value += (harmonic.amplitude / omega) * (std::cos(harmonic.phase) - std::cos((omega * t) + harmonic.phase));
    }
    return base * value;
}

void Workload::WaveSum::add(const Wave& wave) noexcept
{
    base += wave.base;
    for (const auto& harmonic : wave.harmonics)
    {
        // base * a * sin(w t + p) = base * a * (sin(w t) cos(p) + cos(w t) sin(p))
        sinCoefficient[harmonic.band] += wave.base * harmonic.amplitude * std::cos(harmonic.phase);
        cosCoefficient[harmonic.band] += wave.base * harmonic.amplitude * std::sin(harmonic.phase);
    }
}

double Workload::WaveSum::at(double t) const noexcept
{
    double value = base;
    for (std::size_t band = 0; band < BAND_COUNT; ++band)
    {
        const double angle = bandOmega(band) * t;
        value += (sinCoefficient[band] * std::sin(angle)) + (cosCoefficient[band] * std::cos(angle));
    }
    return value;
}

double Workload::WaveSum::integral(double t) const noexcept
{
    // The integral of S sin(w t) + C cos(w t) from 0 to t is S (1 - cos(w t)) / w + C sin(w t) / w.
    double value = base * t;
    for (std::size_t band = 0; band < BAND_COUNT; ++band)
    {
        const double omega = bandOmega(band);
        const double angle = omega * t;
        value += ((sinCoefficient[band] * (1.0 - std::cos(angle))) + (cosCoefficient[band] * std::sin(angle))) / omega;
    }
    return value;
}

// =============================================================================
// Workload
// =============================================================================

Workload::Workload(const WorkloadSpec& spec, std::chrono::steady_clock::time_point epoch, std::uint64_t bootUnixSeconds)
    : m_Spec(spec), m_Epoch(epoch), m_BootUnixSeconds(bootUnixSeconds)
{
    m_Spec.processes = std::clamp(m_Spec.processes, MIN_PROCESSES, MAX_PROCESSES);
    m_Spec.cores = std::clamp(m_Spec.cores, MIN_CORES, MAX_CORES);
    m_Spec.disks = std::min(m_Spec.disks, MAX_DISKS);
    m_Spec.interfaces = std::min(m_Spec.interfaces, MAX_INTERFACES);
    buildProcesses();
    buildSystem();
}

Workload::Workload(const WorkloadSpec& spec)
    : Workload(spec,
               std::chrono::steady_clock::now(),
               static_cast<std::uint64_t>(
                   std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) -
                   static_cast<std::uint64_t>(UPTIME_AT_EPOCH_SECONDS))
{}

double Workload::uptimeAt(std::chrono::steady_clock::time_point time) const noexcept
{
    return UPTIME_AT_EPOCH_SECONDS + std::chrono::duration<double>(time - m_Epoch).count();
}

void Workload::buildProcesses()
{
    // Separate streams, so the system's shape doesn't shift when the process count changes.
    const std::vector<Pending> pending =
        TreeBuilder(m_Spec.processes, m_Spec.cores, m_Spec.seed ^ 0x7072'6f63'6573'7365ULL).build(); // "processe"

    SplitMix64 rng(m_Spec.seed);
    m_Slots.clear();
    m_Slots.reserve(pending.size());
    std::int32_t nextPid = 1;
    std::size_t churnIndex = 0;
    for (const Pending& entry : pending)
    {
        const Profile profile = profileFor(entry.kind);
        Slot slot;
        slot.pid = nextPid;
        nextPid += (entry.kind == Kind::Init) ? 1 : rng.between(1, 6);
        const Slot* parent = (entry.parent >= 0) ? &m_Slots.at(static_cast<std::size_t>(entry.parent)) : nullptr;
        slot.parentPid = (parent != nullptr) ? parent->pid : 0;
        slot.name = entry.name;
        slot.command = entry.command;
        slot.user = entry.user;
        slot.nice = entry.niceSet ? entry.nice : profile.nice;
        slot.kernelThread = (entry.kind == Kind::Kernel);

        const double lifetimeLo = (entry.lifetimeLo >= 0.0) ? entry.lifetimeLo : profile.lifetimeLo;
        const double lifetimeHi = (entry.lifetimeHi >= 0.0) ? entry.lifetimeHi : profile.lifetimeHi;
        if (lifetimeHi > 0.0)
        {
            slot.lifetimeSeconds = rng.uniform(lifetimeLo, lifetimeHi);
            slot.lifetimeOffset = rng.uniform(0.0, slot.lifetimeSeconds);
            slot.churnIndex = churnIndex++;
        }
        else
        {
            const double parentStart = (parent != nullptr) ? parent->startSeconds : 0.0;
            double start = 1.0; // init; never 0, which ProcessTarget reads as "unknown"
            switch (entry.kind)
            {
            case Kind::Init:
                break;
            case Kind::Kernel:
                start = parentStart + rng.uniform(0.01, 2.0);
                break;
            case Kind::Daemon:
            case Kind::ServiceWorker:
                start = parentStart + rng.uniform(1.0, 30.0);
                break;
            case Kind::Session:
                start = parentStart + rng.uniform(5.0, 120.0);
                break;
            default:
                start = parentStart + rng.uniform(5.0, UPTIME_AT_EPOCH_SECONDS / 4.0);
                break;
            }
            slot.startSeconds = std::min(start, std::max(parentStart, MAX_LONG_LIVED_START_SECONDS));
        }

        const bool active = rng.chance(profile.activeChance);
        slot.cpu = makeWave(rng, active ? rng.uniform(profile.cpuLo, profile.cpuHi) : rng.skewed(IDLE_CPU_MIN, IDLE_CPU_MAX, 2.0));
        slot.readBytes = makeWave(rng, rng.skewed(0.0, profile.ioHi, 2.0));
        slot.writeBytes = makeWave(rng, rng.skewed(0.0, profile.ioHi * 0.5, 2.0));
        const bool hasNetwork = rng.chance(profile.netChance);
        const double netReceive = hasNetwork ? rng.skewed(profile.netHi * 0.01, profile.netHi, 2.0) : 0.0;
        slot.netReceived = makeWave(rng, netReceive);
        slot.netSent = makeWave(rng, netReceive * rng.uniform(0.05, 0.6));
        slot.pageFaults = makeWave(rng, rng.skewed(0.0, profile.faultsHi, 2.0));
        slot.rssBytes = rng.skewed(profile.rssMibLo, profile.rssMibHi, 3.0) * MIB;
        slot.rssPhase = rng.uniform(0.0, 2.0 * std::numbers::pi);
        slot.threads = rng.between(profile.threadsLo, std::max(profile.threadsLo, profile.threadsHi));
        slot.handles = rng.between(profile.handlesLo, std::max(profile.handlesLo, profile.handlesHi));
        m_Slots.push_back(std::move(slot));
    }

    // Scale CPU down when all processes together would use more than MAX_PROCESS_CPU_SHARE of the machine.
    double cpuBaseSum = 0.0;
    for (const Slot& slot : m_Slots)
    {
        cpuBaseSum += slot.cpu.base;
    }
    const double cpuBudget = MAX_PROCESS_CPU_SHARE * static_cast<double>(m_Spec.cores);
    const double cpuScale = (cpuBaseSum > cpuBudget) ? cpuBudget / cpuBaseSum : 1.0;

    m_CpuSum = {};
    m_NetSentSum = {};
    m_NetReceivedSum = {};
    m_PageFaultSum = {};
    m_ThreadSum = 0.0;
    m_HandleSum = 0.0;
    m_ProcessRssBytes = 0.0;
    for (Slot& slot : m_Slots)
    {
        slot.cpu.base = std::min(slot.cpu.base * cpuScale, MAX_PROCESS_CPU_BASE);
        m_CpuSum.add(slot.cpu);
        m_NetSentSum.add(slot.netSent);
        m_NetReceivedSum.add(slot.netReceived);
        m_PageFaultSum.add(slot.pageFaults);
        m_ThreadSum += static_cast<double>(slot.threads);
        m_HandleSum += static_cast<double>(slot.handles);
        m_ProcessRssBytes += slot.rssBytes;
    }
}

void Workload::buildSystem()
{
    SplitMix64 rng(m_Spec.seed ^ 0x7379'7374'656d'0000ULL); // "system"

    // Each core runs a share of all process work (slightly uneven, the shares summing to 1) plus its
    // own overhead; with iowait it stays under the core's capacity (see MAX_PROCESS_CPU_SHARE), so
    // idle time always grows and system busy time always covers every process's work.
    m_Cores.clear();
    m_Cores.reserve(m_Spec.cores);
    double shareSum = 0.0;
    for (std::size_t core = 0; core < m_Spec.cores; ++core)
    {
        const double share = 1.0 + rng.uniform(-MAX_CORE_SHARE_SKEW, MAX_CORE_SHARE_SKEW);
        shareSum += share;
        m_Cores.push_back(CoreWaves{.processShare = share,
                                    .overhead = makeWave(rng, rng.uniform(0.002, MAX_CORE_OVERHEAD_BASE)),
                                    .iowait = makeWave(rng, rng.uniform(0.002, MAX_CORE_OVERHEAD_BASE))});
    }
    for (CoreWaves& core : m_Cores)
    {
        core.processShare /= shareSum;
    }

    m_Interfaces.clear();
    m_Interfaces.reserve(m_Spec.interfaces);
    for (std::size_t i = 0; i < m_Spec.interfaces; ++i)
    {
        InterfaceSpec iface;
        switch (i)
        {
        case 0:
            iface.name = "enp5s0";
            iface.linkSpeedMbps = 10'000;
            break;
        case 1:
            iface.name = "wlp4s0";
            iface.linkSpeedMbps = 1'200;
            break;
        case 2:
            iface.name = "docker0";
            iface.isVirtual = true;
            break;
        default:
            // The index makes the name unique (the random suffix has a fixed width, so the index is
            // recoverable); the suffix only makes it look like a real veth name.
            iface.name = std::format("veth{:x}{:06x}", i, rng.next() & 0xFFFFFFULL);
            iface.isVirtual = true;
            break;
        }
        const double rxBase = iface.isVirtual ? rng.skewed(5e4, 5e6, 2.0) : rng.uniform(2e6, 4e7);
        iface.rx = makeWave(rng, rxBase);
        iface.tx = makeWave(rng, rxBase * rng.uniform(0.05, 0.5));
        m_Interfaces.push_back(std::move(iface));
    }

    m_Disks.clear();
    m_Disks.reserve(m_Spec.disks);
    for (std::size_t d = 0; d < m_Spec.disks; ++d)
    {
        m_Disks.push_back(DiskSpec{.name = diskName(d),
                                   .readBytes = makeWave(rng, rng.uniform(2e6, 6e7)),
                                   .writeBytes = makeWave(rng, rng.uniform(1e6, 3e7)),
                                   .busy = makeWave(rng, rng.uniform(0.03, 0.25))});
    }

    // Memory: room for every process's resident set plus cache, in whole 8 GiB steps, at least 16 GiB.
    const double wanted = std::max(16.0 * GIB, m_ProcessRssBytes * 1.6);
    m_TotalMemoryBytes = static_cast<std::uint64_t>(std::ceil(wanted / (8.0 * GIB)) * 8.0 * GIB);
    m_MemoryWave = makeWave(rng, m_ProcessRssBytes * 1.08, 0.08);
}

void Workload::processesAt(double uptime, std::vector<ProcessCounters>& out) const
{
    out.resize(m_Slots.size());
    std::size_t count = 0;
    for (const Slot& slot : m_Slots)
    {
        double start = slot.startSeconds;
        std::int32_t pid = slot.pid;
        if (slot.lifetimeSeconds > 0.0)
        {
            const double generation = std::floor((uptime + slot.lifetimeOffset) / slot.lifetimeSeconds);
            start = (generation * slot.lifetimeSeconds) - slot.lifetimeOffset;
            const auto generationIndex = static_cast<std::int64_t>(generation);
            pid = CHURN_PID_BASE + (static_cast<std::int32_t>(slot.churnIndex) * CHURN_PID_BLOCK) +
                  static_cast<std::int32_t>(((generationIndex % CHURN_PID_BLOCK) + CHURN_PID_BLOCK) % CHURN_PID_BLOCK);
        }
        if (start > uptime)
        {
            continue; // not started yet at this time
        }

        ProcessCounters& counters = out[count++];
        const double cpuSeconds = slot.cpu.integral(uptime) - slot.cpu.integral(start);
        const double cpuNow = slot.cpu.rate(uptime);
        counters.pid = pid;
        counters.parentPid = slot.parentPid;
        counters.name = slot.name;
        counters.command = slot.command;
        counters.user = slot.user;
        if (slot.kernelThread)
        {
            counters.state = (cpuNow > 0.01) ? 'R' : 'I';
        }
        else
        {
            counters.state = (cpuNow > 0.3) ? 'R' : 'S';
        }
        counters.nice = slot.nice;
        counters.startTimeTicks = toCounter(start * TICKS);
        counters.startTimeEpoch = m_BootUnixSeconds + static_cast<std::uint64_t>(std::max(0.0, start));
        counters.userTime = toCounter(cpuSeconds * 0.82 * TICKS);
        counters.systemTime = toCounter(cpuSeconds * 0.18 * TICKS);
        if (slot.kernelThread)
        {
            counters.rssBytes = 0;
            counters.peakRssBytes = 0;
            counters.virtualBytes = 0;
            counters.sharedBytes = 0;
        }
        else
        {
            const double rss = slot.rssBytes * (1.0 + (0.05 * std::sin((uptime * 2.0 * std::numbers::pi / 97.0) + slot.rssPhase)));
            counters.rssBytes = toCounter(rss);
            counters.peakRssBytes = toCounter(slot.rssBytes * 1.05);
            counters.virtualBytes = toCounter((rss * 3.0) + (256.0 * MIB));
            counters.sharedBytes = toCounter(rss * 0.12);
        }
        counters.readBytes = toCounter(slot.readBytes.integral(uptime) - slot.readBytes.integral(start));
        counters.writeBytes = toCounter(slot.writeBytes.integral(uptime) - slot.writeBytes.integral(start));
        counters.threadCount = slot.threads;
        counters.handleCount = slot.handles;
        counters.pageFaultCount = toCounter(slot.pageFaults.integral(uptime) - slot.pageFaults.integral(start));
        counters.netSentBytes = toCounter(slot.netSent.integral(uptime) - slot.netSent.integral(start));
        counters.netReceivedBytes = toCounter(slot.netReceived.integral(uptime) - slot.netReceived.integral(start));
        counters.netSampleTimeNs = 0;
        counters.energyMicrojoules = toCounter(cpuSeconds * WATTS_PER_BUSY_CORE * 1e6);
    }
    out.resize(count);
}

std::uint64_t Workload::totalCpuTicksAt(double uptime) const noexcept
{
    return static_cast<std::uint64_t>(m_Spec.cores) * toCounter(uptime * TICKS);
}

ProcessTotals Workload::processTotalsAt(double uptime) const noexcept
{
    return ProcessTotals{.netSentBytesPerSec = m_NetSentSum.at(uptime),
                         .netReceivedBytesPerSec = m_NetReceivedSum.at(uptime),
                         .pageFaultsPerSec = m_PageFaultSum.at(uptime),
                         .threadCount = m_ThreadSum,
                         .handleCount = m_HandleSum,
                         .powerWatts = m_CpuSum.at(uptime) * WATTS_PER_BUSY_CORE};
}

void Workload::systemCountersAt(double uptime, SystemCounters& out) const
{
    out.cpuTotal = {};
    out.cpuPerCore.resize(m_Cores.size());
    const double processWork = m_CpuSum.integral(uptime); // core-seconds of all process work so far
    const double processNow = m_CpuSum.at(uptime);
    double busyNow = 0.0;
    double busyBase = 0.0;
    for (std::size_t core = 0; core < m_Cores.size(); ++core)
    {
        const CoreWaves& waves = m_Cores[core];
        const double busy = (waves.processShare * processWork) + waves.overhead.integral(uptime);
        const double iowait = waves.iowait.integral(uptime);
        CpuCounters& cpu = out.cpuPerCore[core];
        cpu = {};
        cpu.coreId = core;
        cpu.user = toCounter(busy * 0.70 * TICKS);
        cpu.nice = toCounter(busy * 0.03 * TICKS);
        cpu.system = toCounter(busy * 0.20 * TICKS);
        cpu.irq = toCounter(busy * 0.02 * TICKS);
        cpu.softirq = toCounter(busy * 0.05 * TICKS);
        cpu.iowait = toCounter(iowait * TICKS);
        cpu.idle = toCounter((uptime - busy - iowait) * TICKS);
        out.cpuTotal.user += cpu.user;
        out.cpuTotal.nice += cpu.nice;
        out.cpuTotal.system += cpu.system;
        out.cpuTotal.irq += cpu.irq;
        out.cpuTotal.softirq += cpu.softirq;
        out.cpuTotal.iowait += cpu.iowait;
        out.cpuTotal.idle += cpu.idle;
        busyNow += (waves.processShare * processNow) + waves.overhead.rate(uptime);
        busyBase += (waves.processShare * m_CpuSum.base) + waves.overhead.base;
    }

    const double used = m_MemoryWave.rate(uptime);
    const auto total = static_cast<double>(m_TotalMemoryBytes);
    const double cached = total * (0.18 + (0.02 * std::sin(uptime * 2.0 * std::numbers::pi / 409.0)));
    const double buffers = total * 0.02;
    out.memory.totalBytes = m_TotalMemoryBytes;
    out.memory.availableBytes = toCounter(total - used);
    out.memory.hasAvailableBytes = true;
    out.memory.cachedBytes = toCounter(cached);
    out.memory.buffersBytes = toCounter(buffers);
    out.memory.freeBytes = toCounter(total - used - cached - buffers);
    out.memory.swapTotalBytes = toCounter(8.0 * GIB);
    out.memory.swapFreeBytes = toCounter(8.0 * GIB * (0.92 + (0.04 * std::sin(uptime * 2.0 * std::numbers::pi / 199.0))));

    out.uptimeSeconds = static_cast<std::uint64_t>(uptime);
    out.bootTimestamp = m_BootUnixSeconds;
    out.loadAvg1 = busyNow * 1.1;
    out.loadAvg5 = busyBase * 1.1;
    out.loadAvg15 = busyBase * 1.05;
    out.cpuFreqMHz = toCounter(3600.0 + (800.0 * std::sin(uptime * 2.0 * std::numbers::pi / 23.0)));

    out.networkInterfaces.resize(m_Interfaces.size());
    bool anyPhysical = false;
    for (const auto& iface : m_Interfaces)
    {
        anyPhysical = anyPhysical || !iface.isVirtual;
    }
    out.netRxBytes = 0;
    out.netTxBytes = 0;
    for (std::size_t i = 0; i < m_Interfaces.size(); ++i)
    {
        const InterfaceSpec& spec = m_Interfaces[i];
        auto& iface = out.networkInterfaces[i];
        iface.name = spec.name;
        iface.displayName = spec.name;
        iface.rxBytes = toCounter(spec.rx.integral(uptime));
        iface.txBytes = toCounter(spec.tx.integral(uptime));
        iface.isUp = true;
        iface.linkSpeedMbps = spec.linkSpeedMbps;
        iface.isVirtual = spec.isVirtual;
        iface.isVirtualKnown = true;
        if (!spec.isVirtual || !anyPhysical)
        {
            out.netRxBytes += iface.rxBytes;
            out.netTxBytes += iface.txBytes;
        }
    }

    out.hostname = "tasksmack-synthetic";
    out.cpuModel = "TaskSmack Synthetic CPU";
    out.cpuCoreCount = m_Cores.size();
}

void Workload::diskCountersAt(double uptime, SystemDiskCounters& out) const
{
    constexpr double SECTOR_BYTES = 512.0;
    constexpr double BYTES_PER_OPERATION = 64.0 * 1024.0;
    constexpr double MS_PER_OPERATION = 0.15;
    out.disks.resize(m_Disks.size());
    for (std::size_t d = 0; d < m_Disks.size(); ++d)
    {
        const DiskSpec& spec = m_Disks[d];
        DiskCounters& disk = out.disks[d];
        const double readBytes = spec.readBytes.integral(uptime);
        const double writeBytes = spec.writeBytes.integral(uptime);
        const double busySeconds = spec.busy.integral(uptime);
        disk.deviceName = spec.name;
        disk.readSectors = toCounter(readBytes / SECTOR_BYTES);
        disk.readsCompleted = toCounter(readBytes / BYTES_PER_OPERATION);
        disk.readTimeMs = toCounter(readBytes / BYTES_PER_OPERATION * MS_PER_OPERATION);
        disk.writeSectors = toCounter(writeBytes / SECTOR_BYTES);
        disk.writesCompleted = toCounter(writeBytes / BYTES_PER_OPERATION);
        disk.writeTimeMs = toCounter(writeBytes / BYTES_PER_OPERATION * MS_PER_OPERATION);
        disk.ioInProgressMs = 0;
        disk.ioTimeMs = toCounter(busySeconds * 1000.0);
        disk.weightedIoTimeMs = toCounter(busySeconds * 1500.0);
        disk.sectorSize = static_cast<std::uint64_t>(SECTOR_BYTES);
        disk.isPhysicalDevice = true;
    }
}

ProcessCapabilities Workload::processCapabilities() noexcept
{
    ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasHandleCount = true;
    caps.hasUserSystemTime = true;
    caps.hasStartTime = true;
    caps.hasUser = true;
    caps.hasCommand = true;
    caps.hasNice = true;
    caps.hasPageFaults = true;
    caps.hasPeakRss = true;
    caps.hasNetworkCounters = true;
    caps.hasPowerUsage = true;
    caps.hasSharedMemory = true;
    caps.pageFaultCountBits = 64;
    return caps;
}

SystemCapabilities Workload::systemCapabilities() noexcept
{
    return SystemCapabilities{.hasPerCoreCpu = true,
                              .hasMemoryAvailable = true,
                              .hasSwap = true,
                              .hasUptime = true,
                              .hasIoWait = true,
                              .hasSteal = true,
                              .hasLoadAvg = true,
                              .hasCpuFreq = true,
                              .hasNetworkCounters = true};
}

DiskCapabilities Workload::diskCapabilities() noexcept
{
    return DiskCapabilities{
        .hasDiskStats = true, .hasReadWriteBytes = true, .hasIoTime = true, .hasDeviceInfo = true, .canFilterPhysical = true};
}

} // namespace Platform::Synthetic
