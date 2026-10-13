#pragma once

// NPUs and other compute accelerators on Linux (#1827, the Linux half of #1799), from the kernel's accel
// subsystem: each /sys/class/accel/accelN is one device. Its driver names its kind -- intel_vpu (Intel's
// NPU, "AI Boost") and amdxdna (AMD's XDNA NPU) are NPUs, anything else a compute accelerator -- and
// Intel's driver exports a world-readable busy-time counter (npu_busy_time_us, kernel 6.11+), sampled
// here into a utilization percent. AMD's driver has no unprivileged counter, so it is listed without live
// figures. Neither driver reports per-process utilization; per-process memory (DRM fdinfo) is a follow-up.
// The class root is injectable, so the tests read a fixture tree.

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

namespace AccelNpu
{

/// The share of the time the device was busy between two samples of its busy-time counter, in percent:
/// @p busyDeltaUs microseconds busy out of @p wallDeltaNs nanoseconds, held to 0-100. nullopt without
/// time between them, or when the counter went backwards (the device reset).
[[nodiscard]] std::optional<double> busyPercent(std::uint64_t previousBusyUs, std::uint64_t busyUs, std::uint64_t wallDeltaNs) noexcept;

/// The device's display name for its @p driver: "Intel NPU", "AMD NPU", else "Accelerator (<driver>)".
[[nodiscard]] std::string displayName(const std::string& driver);

/// Its kind: an NPU for intel_vpu and amdxdna, else a compute accelerator.
[[nodiscard]] DeviceKind kindOf(const std::string& driver) noexcept;

} // namespace AccelNpu

class AccelNpuProbe final : public IGPUProbe
{
  public:
    /// Monotonic nanoseconds, the time base the busy counter is sampled against.
    using Clock = std::function<std::uint64_t()>;

    /// @param accelClassRoot where the accel devices are listed (a fixture tree in tests)
    /// @param clock          empty for std::chrono::steady_clock
    explicit AccelNpuProbe(std::filesystem::path accelClassRoot = "/sys/class/accel", Clock clock = {});

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

    /// Whether any accel device was found.
    [[nodiscard]] bool isAvailable() const noexcept
    {
        return !m_Devices.empty();
    }

  private:
    struct Device
    {
        GPUInfo info;
        std::filesystem::path deviceDir; ///< .../accelN/device: the driver's attributes
        std::optional<std::uint64_t> previousBusyUs;
        std::uint64_t previousNs = 0;
    };

    /// The devices listed under the class root now, sorted by name.
    [[nodiscard]] std::vector<Device> scan() const;
    [[nodiscard]] std::uint64_t nowNs() const;

    std::filesystem::path m_Root;
    Clock m_Clock;
    std::vector<Device> m_Devices;
};

} // namespace Platform
