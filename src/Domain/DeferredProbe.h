#pragma once

namespace Domain
{

/// Builds a model before its probe exists (#2731). A panel makes its models on the UI thread, at
/// attach, with this tag; the probes are built on the model's sampler thread (the sampler's start hook,
/// SamplerConfig::onThreadStart) and handed over with the model's setProbe() before its first sample.
/// Until then the model has no probe: it publishes nothing and reports the default capabilities, so
/// the UI draws its empty state.
struct DeferredProbeTag
{
    explicit DeferredProbeTag() = default;
};

inline constexpr DeferredProbeTag DEFERRED_PROBE{};

} // namespace Domain
