#pragma once

#include "IUdpTrafficCollector.h"
#include "ProcessTypes.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace Platform
{

/// One more CPU package's raw energy counter, read with the first (#2444).
struct EnergyCounterReading
{
    std::uint64_t energyUj = 0;
    std::uint64_t maxRangeUj = 0; ///< where the counter wraps back to 0; 0 when unknown
};

/// A raw package energy counter (RAPL on Linux) for Domain to share out between processes per
/// interval (#1093).
struct PackageEnergyReading
{
    std::optional<std::uint64_t> energyUj; ///< nullopt when this sample couldn't be read
    std::uint64_t maxRangeUj = 0;          ///< where the counter wraps back to 0; 0 when unknown
    /// System-wide CPU time spent running processes (user + nice + system), in the same ticks as
    /// ProcessCounters::userTime/systemTime, read with the energy. Unlike the sum over processes
    /// still present, it includes processes that ran and exited between samples, so their share
    /// of the energy isn't charged to the survivors. nullopt when unavailable.
    std::optional<std::uint64_t> busyCpuTicks;
    /// A multi-socket machine's other packages (package-1 and up), whose energy adds to the first's: the
    /// CPU time shared out is every package's (#2444). Empty with one package, or when energyUj is nullopt.
    std::vector<EnergyCounterReading> otherPackages;
};

/// One process's proportional and unique memory (#1885): costly to read (Linux walks every mapping for
/// /proc/[pid]/smaps_rollup), so read for one process at a time -- the one Process Details shows.
struct MemoryRollup
{
    std::uint64_t pssBytes = 0;     ///< Proportional set size: its private pages plus its share of shared ones
    std::uint64_t ussBytes = 0;     ///< Unique set size: the pages only it maps (Private_Clean + Private_Dirty)
    std::uint64_t swapPssBytes = 0; ///< Its proportional share of swapped-out pages
};

/// Why readMemoryRollup() has no figures (#2309).
enum class MemoryRollupFailure : std::uint8_t
{
    Unavailable,      ///< Not readable here: no smaps_rollup, the process is gone, or its PID names another process now
    PermissionDenied, ///< The OS refused (another user's process: smaps_rollup needs ptrace read access)
};

/// One process's PSS and USS, or why there are none.
using MemoryRollupRead = std::expected<MemoryRollup, MemoryRollupFailure>;

/// Interface for platform-specific process enumeration.
/// Implementations read raw counters from OS APIs.
/// Domain layer computes deltas, rates, and percentages.
class IProcessProbe
{
  public:
    virtual ~IProcessProbe() = default;

    IProcessProbe() = default;
    IProcessProbe(const IProcessProbe&) = default;
    IProcessProbe& operator=(const IProcessProbe&) = default;
    IProcessProbe(IProcessProbe&&) = default;
    IProcessProbe& operator=(IProcessProbe&&) = default;

    /// Returns raw counters for all visible processes (stateless read).
    [[nodiscard]] virtual std::vector<ProcessCounters> enumerate() = 0;

    /// What this platform supports.
    [[nodiscard]] virtual ProcessCapabilities capabilities() const = 0;

    /// Total system CPU time (sum of all cores, all states).
    /// Used for calculating per-process CPU%. Called straight after enumerate(), a probe may return
    /// the total it captured at the end of that enumeration's per-process reads, so both cover the
    /// same interval (#1119).
    [[nodiscard]] virtual uint64_t totalCpuTime() const = 0;

    /// Clock ticks per second (e.g., sysconf(_SC_CLK_TCK) on Linux).
    [[nodiscard]] virtual long ticksPerSecond() const = 0;

    /// Total system memory in bytes.
    /// Used for calculating per-process memory%.
    [[nodiscard]] virtual uint64_t systemTotalMemory() const = 0;

    /// Reads the package energy counter that ProcessModel shares out between processes per
    /// interval (stateless; Domain keeps the per-interval state). Returns nullopt when this platform
    /// doesn't use package-energy attribution: ProcessCounters::energyMicrojoules from enumerate()
    /// is then used as-is.
    [[nodiscard]] virtual std::optional<PackageEnergyReading> readPackageEnergy() const
    {
        return std::nullopt;
    }

    /// Reads every connection's raw cumulative byte counters, each attributed to its owning process
    /// (stateless; Domain keeps the per-connection baselines and per-process totals, #1099). Called by
    /// ProcessModel straight after enumerate() and totalCpuTime(). The default returns an empty reading
    /// (sampleTimeNs 0): a probe that doesn't override it reports ProcessCounters::netSentBytes /
    /// netReceivedBytes from enumerate() itself, which are then used as-is.
    [[nodiscard]] virtual SocketTrafficReading readSocketTraffic() const
    {
        return {};
    }

    /// Reads the PSS and USS (#1885) of process @p pid started at @p startTimeTicks (ProcessCounters::
    /// startTimeTicks), where ProcessCapabilities::hasMemoryRollup says the platform can: Unavailable
    /// otherwise, when the process is gone, and when @p pid now names another process
    /// (reused since the snapshot), whose figures would be shown as this one's. Costly, so ProcessModel
    /// reads it only for its watched process (the one Process Details shows).
    [[nodiscard]] virtual MemoryRollupRead readMemoryRollup([[maybe_unused]] std::int32_t pid,
                                                            [[maybe_unused]] std::uint64_t startTimeTicks) const
    {
        return std::unexpected(MemoryRollupFailure::Unavailable);
    }

    /// Process @p pid's record from its /proc files' text (Linux, #2728), as enumerate() would build it from
    /// the files themselves: how a process hidepid hides, read by the helper, joins the list. nullopt where
    /// the platform has no such files, and when its stat doesn't parse.
    [[nodiscard]] virtual std::optional<ProcessCounters> processFromFiles([[maybe_unused]] std::int32_t pid,
                                                                          [[maybe_unused]] const ProcFileTexts& files) const
    {
        return std::nullopt;
    }

    /// Set socket stats cache TTL (Linux only; no-op on Windows).
    /// @param ttlMs Time-to-live in milliseconds for cached socket stats
    /// @note This is a performance optimization cache, separate from the user's refresh interval.
    virtual void setSocketStatsCacheTtl([[maybe_unused]] std::chrono::milliseconds ttlMs)
    {}

    /// Share the opt-in UDP collector (#1492; Windows only), which App starts and stops. While it
    /// delivers, readSocketTraffic() adds each process's UDP bytes and capabilities() reports
    /// hasUdpNetworkCounters. Set once, before the probe is first sampled. The default ignores it.
    virtual void setUdpTrafficCollector([[maybe_unused]] const std::shared_ptr<IUdpTrafficCollector>& collector)
    {}
};

} // namespace Platform
