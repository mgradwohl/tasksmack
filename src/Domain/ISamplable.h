#pragma once

namespace Domain
{

/// Interface for models that can be sampled/refreshed periodically.
class ISamplable
{
  public:
    virtual ~ISamplable() = default;

    ISamplable() = default;
    ISamplable(const ISamplable&) = default;
    ISamplable& operator=(const ISamplable&) = default;
    ISamplable(ISamplable&&) = default;
    ISamplable& operator=(ISamplable&&) = default;

    /// Perform one sampling iteration (should be thread-safe).
    virtual void sample() = 0;

    /// Whether the last sample() could have published anything new to show: BackgroundSampler wakes
    /// the frame loop after a pass only if one did (#3156, #3285). Called on the sampler thread, right
    /// after sample(). A samplable that sometimes reads nothing (a closed App::SamplingGate) answers no.
    [[nodiscard]] virtual bool lastSampleMayHavePublished() const noexcept
    {
        return true;
    }
};

} // namespace Domain
