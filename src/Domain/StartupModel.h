#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Domain/StartupImpact.h"
#include "Platform/IStartupProbe.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Domain
{

/// One immutable generation of the startup entries (#801).
struct StartupPublication
{
    std::uint64_t version = 0;                   ///< 0 until the first sample.
    std::vector<Platform::StartupEntry> entries; ///< Ordered by name, case-insensitively.
    /// Each entry's Startup impact (#3073), by index into entries. Empty when the probe has no impact
    /// data (capabilities().hasImpact false: Linux).
    std::vector<StartupEntryImpact> impacts;
    /// When the measured sign-in began (Unix seconds); 0 when there is no measurement.
    std::uint64_t impactSignInUnixSeconds = 0;
    /// Why every entry is Not measured, when the measurement couldn't be read. Empty otherwise.
    std::string impactUnavailableReason;
};

/// Samples the startup entries off the UI thread (from a BackgroundSampler) and publishes each list
/// as an immutable StartupPublication, so the UI reads it without locking or copying.
class StartupModel : public ISamplable
{
  public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;

    /// @param now When each sample runs, for the impact read's cadence (tests: a fake clock).
    explicit StartupModel(std::unique_ptr<Platform::IStartupProbe> probe, NowFunction now = [] { return Clock::now(); });

    /// Reads the entries and publishes them, each with its Startup impact (#3073). The sign-in
    /// measurement the impact comes from is read with the first sample, then at most every
    /// Sampling::STARTUP_IMPACT_REFRESH_SECONDS, never with every sample. Thread-safe; samples are
    /// serialised.
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
    NowFunction m_Now;
    Platform::StartupCapabilities m_Capabilities;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0;               // guarded by m_SampleMutex
    Platform::StartupImpactReading m_Impact;       // guarded by m_SampleMutex
    std::optional<Clock::time_point> m_ImpactRead; // when m_Impact was read; guarded by m_SampleMutex
    PublicationSlot<StartupPublication> m_Slot;
};

} // namespace Domain
