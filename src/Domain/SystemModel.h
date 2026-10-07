#pragma once

#include "ISamplable.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"
#include "SharedHistory.h"
#include "SystemSnapshot.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// One immutable generation of the system model's state. The histories are views of the model's
/// shared history (#1412): a publish shares their samples rather than copying them, so it costs
/// O(series), not O(history x series). Every series is aligned sample for sample with timestamps.
struct SystemPublication
{
    std::uint64_t version = 0;
    SystemSnapshot snapshot;
    HistoryView<double> timestamps;
    HistoryView<float> cpuHistory;
    HistoryView<float> cpuUserHistory;
    HistoryView<float> cpuSystemHistory;
    HistoryView<float> cpuIowaitHistory;
    HistoryView<float> cpuIdleHistory;
    HistoryView<float> memoryHistory;
    HistoryView<float> memoryCachedHistory;
    HistoryView<float> swapHistory;
    HistoryView<float> powerHistory;
    HistoryView<float> batteryChargeHistory;
    HistoryView<float> netRxHistory;
    HistoryView<float> netTxHistory;
    std::unordered_map<std::string, HistoryView<float>> perInterfaceRxHistory;
    std::unordered_map<std::string, HistoryView<float>> perInterfaceTxHistory;
    std::vector<HistoryView<float>> perCoreHistory; // Indexed by core id (Linux cpuN); NaN where a core had no reading (#1229)
};

/// Owns a system probe, caches previous counters, and computes CPU% deltas.
/// Call refresh() periodically; snapshot() returns the latest computed data.
/// Thread-safe: can receive updates from background sampler. Writers (the update paths and
/// setMaxHistorySeconds()) are serialised on m_WriterMutex; each builds its publication outside
/// every lock a reader takes and swaps it in through m_Publication, so publication() never waits
/// for a history copy (#868).
class SystemModel : public ISamplable
{
  public:
    explicit SystemModel(std::unique_ptr<Platform::ISystemProbe> probe, std::unique_ptr<Platform::IPowerProbe> powerProbe = nullptr);
    ~SystemModel() override = default;

    SystemModel(const SystemModel&) = delete;
    SystemModel& operator=(const SystemModel&) = delete;
    SystemModel(SystemModel&&) = delete;
    SystemModel& operator=(SystemModel&&) = delete;

    /// Perform one sampling iteration (ISamplable implementation).
    void sample() override
    {
        refresh();
    }

    /// Refresh system data from the probe and compute new snapshot.
    /// Thread-safe.
    void refresh();

    /// Update with externally-provided counters (for background sampler).
    /// Thread-safe.
    void updateFromCounters(const Platform::SystemCounters& counters);
    void updateFromCounters(const Platform::SystemCounters& counters, double nowSeconds);

    /// Fills in the next sample of a series: its counters and time (seconds on the same
    /// steady_clock-epoch base as updateFromCounters()), returning false when there are no more.
    using CounterSeriesSource = std::function<bool(Platform::SystemCounters& counters, double& nowSeconds)>;

    /// Applies a series of samples, oldest first, as updateFromCounters() would one at a time, but
    /// under one lock and with one publish at the end: a history preload (the synthetic scenario
    /// fills the whole window at startup, #1413), which one publish per sample would make O(N^2). A
    /// sample not later than the newest one held is skipped, so the history stays in time order.
    /// @p next runs under the model's lock and must not call back into it. Thread-safe.
    void updateFromCounterSeries(const CounterSeriesSource& next);

    /// Get latest computed snapshot (copy for thread safety).
    [[nodiscard]] SystemSnapshot snapshot() const;

    [[nodiscard]] std::shared_ptr<const SystemPublication> publication() const noexcept;
    [[nodiscard]] std::uint64_t publicationVersion() const noexcept;

    /// What the underlying probe supports.
    [[nodiscard]] const Platform::SystemCapabilities& capabilities() const;

    /// Configure maximum retained history duration (seconds), clamped to SamplingConfig's range.
    /// Trims the history to the new window and republishes it at once, once anything has been
    /// published, rather than leaving the old window on show until the next sample (#1145).
    void setMaxHistorySeconds(double seconds);
    /// Thread-safe: read under m_Mutex, which setMaxHistorySeconds() writes it under (#1176).
    [[nodiscard]] double maxHistorySeconds() const;

    /// The network rate ceiling, bytes/s ([metrics] max_sane_rate_bps, shared with ProcessModel, #1291).
    /// An interface rate (or the aggregate fallback rate) above it is taken for a counter glitch: it
    /// reads 0 in the snapshot and is a gap in the history. Clamped to SamplingConfig's range.
    /// Thread-safe; takes effect from the next sample.
    void setMaxSaneNetworkRate(double bytesPerSecond) noexcept;

    // History access (read-only copies)

    [[nodiscard]] std::vector<float> cpuHistory() const;
    [[nodiscard]] std::vector<float> cpuUserHistory() const;
    [[nodiscard]] std::vector<float> cpuSystemHistory() const;
    [[nodiscard]] std::vector<float> cpuIowaitHistory() const;
    [[nodiscard]] std::vector<float> cpuIdleHistory() const;
    [[nodiscard]] std::vector<float> memoryHistory() const;
    [[nodiscard]] std::vector<float> swapHistory() const;
    [[nodiscard]] std::vector<float> memoryCachedHistory() const;
    [[nodiscard]] std::vector<float> powerHistory() const;
    [[nodiscard]] std::vector<float> batteryChargeHistory() const;
    [[nodiscard]] std::vector<float> netRxHistory() const;
    [[nodiscard]] std::vector<float> netTxHistory() const;
    [[nodiscard]] std::vector<float> netRxHistoryForInterface(const std::string& interfaceName) const;
    [[nodiscard]] std::vector<float> netTxHistoryForInterface(const std::string& interfaceName) const;
    [[nodiscard]] std::vector<std::vector<float>> perCoreHistory() const;
    [[nodiscard]] std::vector<double> timestamps() const;

  private:
    std::unique_ptr<Platform::ISystemProbe> m_Probe;
    std::unique_ptr<Platform::IPowerProbe> m_PowerProbe;
    Platform::SystemCapabilities m_Capabilities;
    Platform::PowerCapabilities m_PowerCapabilities;

    // Previous counters for delta calculation
    Platform::SystemCounters m_PrevCounters;
    // Positions of m_PrevCounters' / the current counters' interfaces sorted by name, for O(log n)
    // name lookups instead of linear scans (#1415). Writer-only scratch, reused across samples.
    std::vector<std::size_t> m_PrevInterfaceIndex;
    std::vector<std::size_t> m_InterfaceIndex;
    double m_PrevTimestamp = 0.0;
    bool m_HasPrevious = false;

    // Latest computed snapshot
    SystemSnapshot m_Snapshot;

    // History buffers (shared append-only series, trimmed by time window): publish() hands out views
    // of them instead of copies (#1412)
    SharedHistoryBuffer<float> m_CpuHistory;
    SharedHistoryBuffer<float> m_CpuUserHistory;
    SharedHistoryBuffer<float> m_CpuSystemHistory;
    SharedHistoryBuffer<float> m_CpuIowaitHistory;
    SharedHistoryBuffer<float> m_CpuIdleHistory;
    SharedHistoryBuffer<float> m_MemoryHistory;
    SharedHistoryBuffer<float> m_MemoryCachedHistory;
    SharedHistoryBuffer<float> m_SwapHistory;
    SharedHistoryBuffer<float> m_PowerHistory;
    SharedHistoryBuffer<float> m_BatteryChargeHistory;
    SharedHistoryBuffer<float> m_NetRxHistory;
    SharedHistoryBuffer<float> m_NetTxHistory;
    // Per-interface network history (keyed by interface name)
    std::unordered_map<std::string, SharedHistoryBuffer<float>> m_PerInterfaceRxHistory;
    std::unordered_map<std::string, SharedHistoryBuffer<float>> m_PerInterfaceTxHistory;
    // Wall-clock time (nowSeconds, same clock as m_Timestamps) each interface name was last
    // seen in a live sample, so a name absent for longer than the configured history window
    // (at which point its buffers hold nothing but NaN padding) can be pruned instead of
    // retained forever -- otherwise a machine with churning interfaces (container veth*/br-*,
    // VPN reconnects, WiFi cycling) leaks one HistoryBuffer pair per distinct interface name
    // ever seen (#776). Deliberately time-based, matching trimHistory()'s own cutoff, rather
    // than counting refresh cycles: historyCapacityForSeconds() sizes ring buffers for the
    // fastest *supported* refresh cadence, not the actual one, so a cycle-count threshold
    // could retain stale entries far longer than m_MaxHistorySeconds at any slower cadence
    // (e.g. ~10x longer at the default 1s refresh / 5 minute window).
    std::unordered_map<std::string, double> m_InterfaceLastSeenSeconds;
    SharedHistoryBuffer<double> m_Timestamps;
    std::vector<SharedHistoryBuffer<float>> m_PerCoreHistory; // Indexed by core id, not probe list position (#1229)
    std::vector<std::size_t> m_SeenCoreIds;                   // Every core id reported this session, ascending (#1262)

    double m_MaxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT; // Default 5 minutes
    std::atomic<double> m_MaxSaneNetworkRateBps{Sampling::MAX_SANE_RATE_BPS_DEFAULT};

    PublicationSlot<SystemPublication> m_Publication;
    std::uint64_t m_PublicationVersion = 0; // guarded by m_WriterMutex; the last committed generation

    // Thread safety. m_WriterMutex serialises the writers from the counter processing through the
    // publication commit, so generations are numbered and committed in order; readers never take
    // it, and it is taken before m_Mutex, never while holding it. m_Mutex guards the state above for
    // snapshot() and the per-field accessors: writers mutate it exclusively, and publish() reads it
    // under a shared lock.
    std::mutex m_WriterMutex;
    mutable std::shared_mutex m_Mutex;

    // Helpers
    // Locks once and applies both the power reading (if any) and the counter-derived
    // snapshot atomically, so readers never observe one cycle's power paired with the
    // previous cycle's CPU/memory/network data.
    void
    updateFromCountersLocked(const Platform::SystemCounters& counters, double nowSeconds, const std::optional<PowerStatus>& powerStatus);
    /// Computes one sample and applies it to the snapshot and history as a transaction with the strong
    /// guarantee: everything that can throw (std::bad_alloc) runs before anything is changed, so a throw
    /// here leaves the snapshot and history as they were -- every series still aligned with
    /// m_Timestamps (#1412). The publish() that follows can still throw while building the
    /// publication; that leaves the history one sample ahead of an unchanged publication and version,
    /// which the next successful publish catches up.
    void computeSnapshot(const Platform::SystemCounters& counters,
                         double nowSeconds,
                         const std::optional<PowerStatus>& powerStatus = std::nullopt);
    /// Build the next generation from the history state under a shared lock, then commit it.
    /// Strong guarantee: a throw while building leaves the publication and its version unchanged.
    /// Requires m_WriterMutex held and m_Mutex not held.
    void publish();
    void trimHistory(double nowSeconds) noexcept;

    /// One sample's history append, staged: every allocation it needs is made here, so applying it
    /// cannot fail part way (#1412).
    struct PendingHistory
    {
        float netRx = 0.0F; // the history values (NaN for a glitched rate, #1291)
        float netTx = 0.0F;
        std::vector<float> interfaceRx; // per counters.networkInterfaces entry, likewise
        std::vector<float> interfaceTx;
        std::vector<SharedHistoryBuffer<float>> newCores;                  // slots for core ids not yet held, backfilled
        std::unordered_map<std::string, SharedHistoryBuffer<float>> newRx; // new interfaces, backfilled
        std::unordered_map<std::string, SharedHistoryBuffer<float>> newTx;
        std::unordered_map<std::string, double> newLastSeen;
    };
    /// Create and backfill the series this sample introduces (new core slots, new interfaces) in
    /// @p pending, and reserve room for one more sample in every existing series and the slots,
    /// buckets and map entries the append will use. May throw; changes nothing observable.
    /// Requires m_Mutex held exclusively.
    void stageHistoryAppend(PendingHistory& pending,
                            const Platform::SystemCounters& counters,
                            const SystemSnapshot& snap,
                            std::size_t coreSlots,
                            double nowSeconds);
    /// Apply a staged append: adopt the staged series and append this sample to every series. Uses
    /// only what stageHistoryAppend() reserved, so it does not allocate or throw.
    void commitHistoryAppend(PendingHistory& pending,
                             const Platform::SystemCounters& counters,
                             const SystemSnapshot& snap,
                             double nowSeconds) noexcept;
    void applyHistoryCapacity();
    [[nodiscard]] static CpuUsage computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous);
    [[nodiscard]] PowerStatus computePowerStatus(const Platform::PowerCounters& counters) const;

    /// Find a previous interface by name for rate calculation: a binary search of m_PrevInterfaceIndex.
    [[nodiscard]] const Platform::SystemCounters::InterfaceCounters* findPreviousInterface(const std::string& name) const;
};

} // namespace Domain
