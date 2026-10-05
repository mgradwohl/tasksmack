#pragma once

#include "GPUTypes.h"

#include <memory>
#include <vector>

namespace Platform
{

class IGPUProbe
{
  public:
    virtual ~IGPUProbe() = default;

    IGPUProbe() = default;
    IGPUProbe(const IGPUProbe&) = default;
    IGPUProbe& operator=(const IGPUProbe&) = default;
    IGPUProbe(IGPUProbe&&) = default;
    IGPUProbe& operator=(IGPUProbe&&) = default;

    // Enumerate available GPUs: at startup, and again whenever rescanGPUs() says the result may have
    // changed. Must not wake a runtime-suspended GPU on a repeat call (#1117).
    [[nodiscard]] virtual std::vector<GPUInfo> enumerateGPUs() = 0;

    /// Look for changes since the last enumerateGPUs() and say whether it should be called again
    /// (#1116, #1289). GPUModel calls this on its sampler thread, under the same lock as every other
    /// probe call, before each readGPUCounters().
    ///  - GPURescan::Quick (every sample) must be cheap: report only what the probe already knows or
    ///    can tell from runtime-PM bookkeeping, e.g. an adapter that was asleep at enumeration, so
    ///    its own sensor set is still unknown, is now awake (#1289).
    ///  - GPURescan::Full (at a low rate, Sampling::GPU_RESCAN_INTERVAL_SECONDS) may also look for
    ///    hot-plugged, removed or lost devices and rebuild the probe's device list (re-initialise the
    ///    vendor library, rescan sysfs) so that the next readGPUCounters() and enumerateGPUs() see
    ///    the new set. A GPU that persists keeps its id, so its history carries on.
    /// Neither may wake a runtime-suspended GPU. The default reports no change: the device set is
    /// fixed at construction (the Windows probes today).
    [[nodiscard]] virtual bool rescanGPUs(GPURescan depth)
    {
        static_cast<void>(depth);
        return false;
    }

    // Read system-level GPU metrics (called every sample interval)
    [[nodiscard]] virtual std::vector<GPUCounters> readGPUCounters() = 0;

    // Read per-process GPU metrics (called every sample interval)
    // Returns empty vector if not supported
    [[nodiscard]] virtual std::vector<ProcessGPUCounters> readProcessGPUCounters() = 0;

    // Capability reporting
    [[nodiscard]] virtual GPUCapabilities capabilities() const = 0;
};

} // namespace Platform
