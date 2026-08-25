#pragma once

#include "Platform/GPUTypes.h"

#include <memory>
#include <vector>

namespace Platform
{

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
    PDHGPUProbe();
    ~PDHGPUProbe();

    // Non-copyable, movable
    PDHGPUProbe(const PDHGPUProbe&) = delete;
    PDHGPUProbe& operator=(const PDHGPUProbe&) = delete;
    PDHGPUProbe(PDHGPUProbe&&) noexcept;
    PDHGPUProbe& operator=(PDHGPUProbe&&) noexcept;

    /// @brief Check if PDH GPU counters are available
    [[nodiscard]] bool isAvailable() const;

    /// @brief Read per-process GPU utilization counters
    /// @return Vector of per-process GPU counters with utilization percentages
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters();

    /// @brief Get capabilities of this probe
    [[nodiscard]] GPUCapabilities capabilities() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
