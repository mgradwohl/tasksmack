#pragma once

// NPUs and other compute accelerators on Linux (#1827, the Linux half of #1799), from the kernel's accel
// subsystem: each /sys/class/accel/accelN is one device. Its driver names its kind -- intel_vpu (Intel's
// NPU, "AI Boost") and amdxdna (AMD's XDNA NPU) are NPUs, anything else a compute accelerator -- and
// Intel's driver exports a world-readable busy-time counter (npu_busy_time_us, kernel 6.11+), sampled
// here into a utilization percent. AMD's driver has no unprivileged counter, so it is listed without live
// figures. Neither driver reports per-process utilization. Per-process memory comes from DRM fdinfo
// (#1827 PR 2b): the processes holding /dev/accel/accelN open are found by a walk of /proc/*/fd every
// CLIENT_DISCOVERY_INTERVAL_NS, and each sample reads only those clients' fdinfo -- drm-resident-memory
// (or drm-total-memory), once per drm-client-id however many fds share it. The NPUs map system memory,
// so it is the processes' shared memory. The class and proc roots are injectable, so the tests read
// fixture trees.

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

/// One accel client's fdinfo (/proc/<pid>/fdinfo/<fd>, the kernel's drm-usage-stats format), as
/// parseFdinfo() reads it.
struct ClientMemory
{
    std::optional<std::uint64_t> clientId; ///< drm-client-id: fds sharing one are one client
    std::optional<std::uint64_t> bytes;    ///< drm-resident-memory, else drm-total-memory, in bytes
};

/// Parses an accel fd's fdinfo @p text. A memory value is "<n>", "<n> KiB", "<n> MiB" or "<n> GiB"; one
/// in any other form, or too large, is left out.
[[nodiscard]] ClientMemory parseFdinfo(std::string_view text);

} // namespace AccelNpu

class AccelNpuProbe final : public IGPUProbe
{
  public:
    /// Monotonic nanoseconds, the time base the busy counter is sampled against.
    using Clock = std::function<std::uint64_t()>;

    /// How often the processes holding an accel device open are looked for again (#1827): a walk of
    /// every process's fd directory, so not every sample. A process that opens the NPU shows from the
    /// next walk on; one that closes it, from its next fdinfo read.
    static constexpr std::uint64_t CLIENT_DISCOVERY_INTERVAL_NS = 5'000'000'000ULL;

    /// @param accelClassRoot where the accel devices are listed (a fixture tree in tests)
    /// @param clock          empty for std::chrono::steady_clock
    /// @param procRoot       where the accel clients' fds and fdinfo are looked for (a fixture tree in tests)
    explicit AccelNpuProbe(std::filesystem::path accelClassRoot = "/sys/class/accel",
                           Clock clock = {},
                           std::filesystem::path procRoot = "/proc");

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
        std::string nodePath;            ///< /dev/accel/accelN, as a client's fd link names it
        std::optional<std::uint64_t> previousBusyUs;
        std::uint64_t previousNs = 0;
    };

    /// One process's fd open on one of m_Devices.
    struct Client
    {
        std::int32_t pid = 0;
        std::filesystem::path fdinfo; ///< <proc>/<pid>/fdinfo/<fd>
        std::size_t device = 0;       ///< Index into m_Devices
    };

    /// The devices listed under the class root now, sorted by name.
    [[nodiscard]] std::vector<Device> scan() const;
    [[nodiscard]] std::uint64_t nowNs() const;
    /// Walks m_ProcRoot for the fds open on m_Devices' nodes into m_Clients.
    void discoverClients();

    std::filesystem::path m_Root;
    Clock m_Clock;
    std::filesystem::path m_ProcRoot;
    std::vector<Device> m_Devices;
    std::vector<Client> m_Clients;
    std::optional<std::uint64_t> m_LastDiscoveryNs; ///< nullopt until the first walk, and after m_Devices changes
};

} // namespace Platform
