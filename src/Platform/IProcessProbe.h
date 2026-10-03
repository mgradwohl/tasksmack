#pragma once

#include "ProcessTypes.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace Platform
{

/// A raw package energy counter (RAPL on Linux) for Domain to share out between processes per
/// interval (#1093).
struct PackageEnergyReading
{
    std::optional<std::uint64_t> energyUj; ///< nullopt when this sample couldn't be read
    std::uint64_t maxRangeUj = 0;          ///< where the counter wraps back to 0; 0 when unknown
};

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
    /// Used for calculating per-process CPU%.
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

    /// Set socket stats cache TTL (Linux only; no-op on Windows).
    /// @param ttlMs Time-to-live in milliseconds for cached socket stats
    /// @note This is a performance optimization cache, separate from the user's refresh interval.
    virtual void setSocketStatsCacheTtl([[maybe_unused]] std::chrono::milliseconds ttlMs)
    {}
};

} // namespace Platform
