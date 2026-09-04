#pragma once

#include "History.h"
#include "ISamplable.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "SamplingConfig.h"
#include "SystemSnapshot.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Domain
{

struct SystemPublication
{
    std::uint64_t version = 0;
    SystemSnapshot snapshot;
    std::vector<double> timestamps;
    std::vector<float> cpuHistory;
    std::vector<float> cpuUserHistory;
    std::vector<float> cpuSystemHistory;
    std::vector<float> cpuIowaitHistory;
    std::vector<float> cpuIdleHistory;
    std::vector<float> memoryHistory;
    std::vector<float> memoryCachedHistory;
    std::vector<float> swapHistory;
    std::vector<float> powerHistory;
    std::vector<float> batteryChargeHistory;
    std::vector<float> netRxHistory;
    std::vector<float> netTxHistory;
    std::unordered_map<std::string, std::vector<float>> perInterfaceRxHistory;
    std::unordered_map<std::string, std::vector<float>> perInterfaceTxHistory;
    std::vector<std::vector<float>> perCoreHistory;
};

/// Owns a system probe, caches previous counters, and computes CPU% deltas.
/// Call refresh() periodically; snapshot() returns the latest computed data.
/// Thread-safe: can receive updates from background sampler.
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

    /// Get latest computed snapshot (copy for thread safety).
    [[nodiscard]] SystemSnapshot snapshot() const;

    [[nodiscard]] std::shared_ptr<const SystemPublication> publication() const noexcept;
    [[nodiscard]] std::uint64_t publicationVersion() const noexcept;

    /// What the underlying probe supports.
    [[nodiscard]] const Platform::SystemCapabilities& capabilities() const;

    /// Configure maximum retained history duration (seconds).
    void setMaxHistorySeconds(double seconds);
    [[nodiscard]] double maxHistorySeconds() const
    {
        return m_MaxHistorySeconds;
    }

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
    double m_PrevTimestamp = 0.0;
    bool m_HasPrevious = false;

    // Latest computed snapshot
    SystemSnapshot m_Snapshot;

    // History buffers (runtime-capacity ring buffers, trimmed by time window)
    HistoryBuffer<float> m_CpuHistory;
    HistoryBuffer<float> m_CpuUserHistory;
    HistoryBuffer<float> m_CpuSystemHistory;
    HistoryBuffer<float> m_CpuIowaitHistory;
    HistoryBuffer<float> m_CpuIdleHistory;
    HistoryBuffer<float> m_MemoryHistory;
    HistoryBuffer<float> m_MemoryCachedHistory;
    HistoryBuffer<float> m_SwapHistory;
    HistoryBuffer<float> m_PowerHistory;
    HistoryBuffer<float> m_BatteryChargeHistory;
    HistoryBuffer<float> m_NetRxHistory;
    HistoryBuffer<float> m_NetTxHistory;
    // Per-interface network history (keyed by interface name)
    std::unordered_map<std::string, HistoryBuffer<float>> m_PerInterfaceRxHistory;
    std::unordered_map<std::string, HistoryBuffer<float>> m_PerInterfaceTxHistory;
    // Wall-clock time (nowSeconds, same clock as m_Timestamps) each interface name was last
    // seen in a live sample, so a name absent for longer than the configured history window
    // (at which point its buffers hold nothing but zero padding) can be pruned instead of
    // retained forever -- otherwise a machine with churning interfaces (container veth*/br-*,
    // VPN reconnects, WiFi cycling) leaks one HistoryBuffer pair per distinct interface name
    // ever seen (#776). Deliberately time-based, matching trimHistory()'s own cutoff, rather
    // than counting refresh cycles: historyCapacityForSeconds() sizes ring buffers for the
    // fastest *supported* refresh cadence, not the actual one, so a cycle-count threshold
    // could retain stale entries far longer than m_MaxHistorySeconds at any slower cadence
    // (e.g. ~10x longer at the default 1s refresh / 5 minute window).
    std::unordered_map<std::string, double> m_InterfaceLastSeenSeconds;
    HistoryBuffer<double> m_Timestamps;
    std::vector<HistoryBuffer<float>> m_PerCoreHistory;

    double m_MaxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT; // Default 5 minutes

    std::shared_ptr<const SystemPublication> m_Publication = std::make_shared<const SystemPublication>();
    std::uint64_t m_PublicationVersion = 0;
    std::atomic<std::uint64_t> m_PublishedPublicationVersion{0};

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helpers
    // Locks once and applies both the power reading (if any) and the counter-derived
    // snapshot atomically, so readers never observe one cycle's power paired with the
    // previous cycle's CPU/memory/network data.
    void
    updateFromCountersLocked(const Platform::SystemCounters& counters, double nowSeconds, const std::optional<PowerStatus>& powerStatus);
    void computeSnapshot(const Platform::SystemCounters& counters, double nowSeconds);
    void publish();
    void trimHistory(double nowSeconds);
    void applyHistoryCapacity();
    [[nodiscard]] static CpuUsage computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous);
    [[nodiscard]] PowerStatus computePowerStatus(const Platform::PowerCounters& counters) const;

    /// Find a previous interface by name for rate calculation.
    [[nodiscard]] const Platform::SystemCounters::InterfaceCounters* findPreviousInterface(const std::string& name) const;
};

} // namespace Domain
