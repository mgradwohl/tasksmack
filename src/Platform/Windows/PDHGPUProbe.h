#pragma once

#include "Platform/GPUTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Platform
{

/// GPU memory in use on one adapter, from the PDH "GPU Adapter Memory" counters.
struct AdapterMemoryUsage
{
    std::uint64_t dedicatedBytes = 0;
    std::uint64_t sharedBytes = 0;
    /// Whether this collect read each segment for the adapter: its counter array was read and had a
    /// good item for the adapter, even one of 0 bytes. A 0 in a segment not read is not a reading,
    /// and a 0 in one that was read is a real 0 B (#1246).
    bool dedicatedRead = false;
    bool sharedRead = false;
};

/// @brief PDH-based GPU probe for per-process GPU engine utilization
///
/// Uses Windows Performance Counters (PDH) to query GPU Engine utilization.
/// This is the same mechanism Task Manager uses for per-process GPU %.
///
/// Counter category: "GPU Engine"
/// Counter: "Utilization Percentage"
/// Instance format: pid_<PID>_luid_<LUID>_phys_<N>_eng_<N>_engtype_<Type>
///
/// Uses persistent wildcard counters ("GPU Engine(*)"), which PDH re-expands on
/// every collection, so new GPU-using processes are discovered automatically each
/// sample without expensive instance re-enumeration.
class PDHGPUProbe
{
  public:
    struct Impl;

    /// Which figures this probe's query collects, so each query adds only the counters it uses
    /// (#1175). Both add "GPU Engine(*)": PDH computes rates per query between its own collects,
    /// and the process and system samplers run at different intervals, so neither query can
    /// borrow the other's engine rates (#1034).
    enum class Role : std::uint8_t
    {
        /// Per-process utilization and memory: "GPU Engine" and "GPU Process Memory".
        Process,
        /// Adapter utilization and memory in use: "GPU Engine" and "GPU Adapter Memory". Its
        /// readProcessGPUCounters() returns no processes; it refreshes adapterUtilization() and
        /// adapterMemory().
        Adapter,
    };

    explicit PDHGPUProbe(Role role = Role::Process);
    ~PDHGPUProbe();

    /// Test-only: construct around a pre-populated Impl (e.g. with an injected fake PDH
    /// function table in place of the real pdh.dll exports), bypassing normal pdh.dll loading.
    /// Requires "Platform/Windows/PDHGPUProbeImpl.h" for Impl's full definition; production
    /// code should always use the default constructor.
    explicit PDHGPUProbe(std::unique_ptr<Impl> impl);

    // Non-copyable, movable
    PDHGPUProbe(const PDHGPUProbe&) = delete;
    PDHGPUProbe& operator=(const PDHGPUProbe&) = delete;
    PDHGPUProbe(PDHGPUProbe&&) noexcept;
    PDHGPUProbe& operator=(PDHGPUProbe&&) noexcept;

    /// @brief Check if PDH GPU counters are available
    [[nodiscard]] bool isAvailable() const;

    /// @brief Collect this probe's query and read per-process GPU utilization counters
    /// @return Vector of per-process GPU counters with utilization percentages; always empty for
    ///         Role::Adapter, which reads only the adapter figures
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters();

    /// Per-adapter GPU utilization (Role::Adapter only) from the most recent readProcessGPUCounters() call, keyed by
    /// "GPU_<luid>" (DXGI's luidId format). For each engine the sum over processes, then the
    /// busiest engine, as Task Manager defines it (#1033). Empty -- unread -- after a warm-up collect
    /// (no rates yet) and after any failed collect, so neither publishes 0% or a stale reading (#1166).
    [[nodiscard]] std::unordered_map<std::string, double> adapterUtilization() const;
    /// Whether adapterUtilization() comes from a successful, warmed-up collect. When it does, an
    /// adapter absent from it had no GPU Engine instances -- nothing ran on it -- so it is idle (0%),
    /// not unread; when it does not (warm-up, failed collect), every adapter is unread (#1166).
    [[nodiscard]] bool adapterUtilizationCurrent() const;
    /// Adapters ("GPU_<luid>") that had GPU Engine items in the last collect but none with a
    /// readable value: unread (a gap), never idle, even when adapterUtilizationCurrent() (#1166).
    [[nodiscard]] std::unordered_set<std::string> adapterUtilizationUnread() const;

    /// Adapter-wide GPU memory in use (Role::Adapter only) from the most recent readProcessGPUCounters() call, keyed by
    /// "GPU_<luid>". Unlike DXGI's QueryVideoMemoryInfo, which reports only the calling process,
    /// this covers every process on the adapter (#1029). Empty after a failed collect (#1166).
    [[nodiscard]] std::unordered_map<std::string, AdapterMemoryUsage> adapterMemory() const;

    /// @brief Get capabilities of this probe
    [[nodiscard]] GPUCapabilities capabilities() const;

    /// Diagnostic snapshot of instanceFor()'s instance-name cache behavior, added while
    /// investigating perf-plan-574 / issue #583. See PDHGPUProbe::Impl::instanceCacheHits
    /// et al. for what each counter tracks.
    struct CacheStats
    {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t clears = 0;
        std::size_t cachedEntries = 0;
        /// Positional-slot shortcut counters (instanceForAt()): a positional hit skips the
        /// hash-map lookup entirely, so these are disjoint from hits/misses above, not a
        /// subset of them.
        std::uint64_t positionalHits = 0;
        std::uint64_t positionalMisses = 0;
    };

    /// @brief Snapshot of the instance-name cache's cumulative hit/miss/clear counts and
    /// current size. Cheap (a handful of field reads); safe to call at any time, including
    /// before the probe is available.
    [[nodiscard]] CacheStats instanceCacheStats() const;

  private:
    /// Role::Adapter's reads after a collect: adapter memory in use (every collect, it is a
    /// gauge) and adapter utilization (once warmed up). Neither builds per-process aggregates.
    void readAdapterMemory();
    void readAdapterUtilization();

    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
