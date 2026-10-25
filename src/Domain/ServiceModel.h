#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IServiceProbe.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// Indices into a service list of the services each PID hosts (#1880), in the list's order. A
/// stopped service (PID 0) is in no entry.
using ServicesByPid = std::unordered_map<std::uint32_t, std::vector<std::size_t>>;

/// The services in @p services that each PID hosts. Several can share one process (an svchost.exe);
/// a PID with none has no entry.
[[nodiscard]] ServicesByPid indexServicesByPid(std::span<const Platform::ServiceInfo> services);

/// One immutable generation of the service list (#800).
struct ServicePublication
{
    std::uint64_t version = 0;                   ///< 0 until the first sample.
    std::vector<Platform::ServiceInfo> services; ///< Ordered by name, case-insensitively.
    /// The services each PID hosts (#1880), built with the list on the sampler thread, so a reader
    /// looks a process up rather than scanning the list.
    ServicesByPid servicesByPid;
    /// When `services` was read: the processes listed since may have reused a PID it names (#1880).
    /// For a stale generation, the last good read's time. The epoch before the first sample.
    std::chrono::steady_clock::time_point captureTime;

    /// The latest read failed: `services` is the last list that was read (empty if none ever was),
    /// kept rather than replaced by an empty one, and failureReason says why it is out of date.
    bool stale = false;
    std::string failureReason;
};

/// The services @p publication lists as hosted by @p pid, as indices into its `services`; empty for
/// none (#1880).
[[nodiscard]] std::span<const std::size_t> servicesHostedBy(const ServicePublication& publication, std::uint32_t pid) noexcept;

/// The service @p publication lists under @p name (its exact name), or nullptr.
[[nodiscard]] const Platform::ServiceInfo* findService(const ServicePublication& publication, std::string_view name) noexcept;

/// Samples the services off the UI thread (from a BackgroundSampler) and publishes each list as an
/// immutable ServicePublication, so the UI reads it without locking or copying.
class ServiceModel : public ISamplable
{
  public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;

    /// @param now Stamps each read's captureTime (tests: a fake clock, as ProcessModel takes one).
    explicit ServiceModel(std::unique_ptr<Platform::IServiceProbe> probe, NowFunction now = [] { return Clock::now(); });

    /// Reads the services and publishes them. A failed read publishes the previous list again,
    /// marked stale with the reason. Thread-safe; samples are serialised.
    void sample() override;

    /// The newest generation (an empty version-0 one before the first sample).
    [[nodiscard]] std::shared_ptr<const ServicePublication> publication() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// Has the next sample re-read every service's configuration rather than wait for the probe's
    /// cache to expire: a service action may have changed one (#1577). Thread-safe.
    void requestConfigReread() noexcept
    {
        m_ConfigRereadRequested.store(true, std::memory_order_release);
    }

    /// What the probe can read, fixed at construction.
    [[nodiscard]] const Platform::ServiceCapabilities& capabilities() const noexcept
    {
        return m_Capabilities;
    }

  private:
    std::unique_ptr<Platform::IServiceProbe> m_Probe; // used under m_SampleMutex only
    NowFunction m_Now;
    Platform::ServiceCapabilities m_Capabilities;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    std::atomic<bool> m_ConfigRereadRequested{false};
    PublicationSlot<ServicePublication> m_Slot;
};

} // namespace Domain
