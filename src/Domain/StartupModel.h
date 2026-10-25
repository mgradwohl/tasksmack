#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Platform/IStartupProbe.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Domain
{

/// One immutable generation of the startup entries (#801).
struct StartupPublication
{
    std::uint64_t version = 0;                   ///< 0 until the first sample.
    std::vector<Platform::StartupEntry> entries; ///< Ordered by name, case-insensitively.
};

/// Samples the startup entries off the UI thread (from a BackgroundSampler) and publishes each list
/// as an immutable StartupPublication, so the UI reads it without locking or copying.
class StartupModel : public ISamplable
{
  public:
    explicit StartupModel(std::unique_ptr<Platform::IStartupProbe> probe);

    /// Reads the entries and publishes them. Thread-safe; samples are serialised.
    void sample() override;

    /// The newest generation (an empty version-0 one before the first sample).
    [[nodiscard]] std::shared_ptr<const StartupPublication> publication() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// What the probe can read, fixed at construction.
    [[nodiscard]] const Platform::StartupCapabilities& capabilities() const noexcept
    {
        return m_Capabilities;
    }

  private:
    std::unique_ptr<Platform::IStartupProbe> m_Probe; // used under m_SampleMutex only
    Platform::StartupCapabilities m_Capabilities;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    PublicationSlot<StartupPublication> m_Slot;
};

} // namespace Domain
