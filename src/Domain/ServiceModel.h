#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IServiceProbe.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Domain
{

/// One immutable generation of the service list (#800).
struct ServicePublication
{
    std::uint64_t version = 0;                   ///< 0 until the first sample.
    std::vector<Platform::ServiceInfo> services; ///< Ordered by name, case-insensitively.
};

/// Samples the services off the UI thread (from a BackgroundSampler) and publishes each list as an
/// immutable ServicePublication, so the UI reads it without locking or copying.
class ServiceModel : public ISamplable
{
  public:
    explicit ServiceModel(std::unique_ptr<Platform::IServiceProbe> probe);

    /// Reads the services and publishes them. Thread-safe; samples are serialised.
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

    /// What the probe can read, fixed at construction.
    [[nodiscard]] const Platform::ServiceCapabilities& capabilities() const noexcept
    {
        return m_Capabilities;
    }

  private:
    std::unique_ptr<Platform::IServiceProbe> m_Probe; // used under m_SampleMutex only
    Platform::ServiceCapabilities m_Capabilities;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    PublicationSlot<ServicePublication> m_Slot;
};

} // namespace Domain
