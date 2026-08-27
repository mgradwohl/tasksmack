#pragma once

#include "History.h"
#include "ISamplable.h"
#include "Platform/IPowerProbe.h"
#include "Platform/ISystemProbe.h"
#include "SamplingConfig.h"
#include "SystemSnapshot.h"

#include <atomic>
#include <deque>
#include <memory>
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

    // History buffers (fixed-size ring buffers - no trimming needed)
    History<float, HistoryCapacity::STANDARD> m_CpuHistory;
    History<float, HistoryCapacity::STANDARD> m_CpuUserHistory;
    History<float, HistoryCapacity::STANDARD> m_CpuSystemHistory;
    History<float, HistoryCapacity::STANDARD> m_CpuIowaitHistory;
    History<float, HistoryCapacity::STANDARD> m_CpuIdleHistory;
    History<float, HistoryCapacity::STANDARD> m_MemoryHistory;
    History<float, HistoryCapacity::STANDARD> m_MemoryCachedHistory;
    History<float, HistoryCapacity::STANDARD> m_SwapHistory;
    History<float, HistoryCapacity::STANDARD> m_PowerHistory;
    History<float, HistoryCapacity::STANDARD> m_BatteryChargeHistory;
    History<float, HistoryCapacity::STANDARD> m_NetRxHistory;
    History<float, HistoryCapacity::STANDARD> m_NetTxHistory;
    // Per-interface network history (keyed by interface name)
    std::unordered_map<std::string, History<float, HistoryCapacity::STANDARD>> m_PerInterfaceRxHistory;
    std::unordered_map<std::string, History<float, HistoryCapacity::STANDARD>> m_PerInterfaceTxHistory;
    History<double, HistoryCapacity::STANDARD> m_Timestamps;
    std::vector<History<float, HistoryCapacity::STANDARD>> m_PerCoreHistory;

    double m_MaxHistorySeconds = Domain::Sampling::HISTORY_SECONDS_DEFAULT; // Default 5 minutes

    std::shared_ptr<const SystemPublication> m_Publication = std::make_shared<const SystemPublication>();
    std::uint64_t m_PublicationVersion = 0;
    std::atomic<std::uint64_t> m_PublishedPublicationVersion{0};

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helpers
    void computeSnapshot(const Platform::SystemCounters& counters, double nowSeconds);
    void publish();
    void trimHistory(double nowSeconds);
    [[nodiscard]] static CpuUsage computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous);
    [[nodiscard]] PowerStatus computePowerStatus(const Platform::PowerCounters& counters) const;

    /// Find a previous interface by name for rate calculation.
    [[nodiscard]] const Platform::SystemCounters::InterfaceCounters* findPreviousInterface(const std::string& name) const;
};

} // namespace Domain
