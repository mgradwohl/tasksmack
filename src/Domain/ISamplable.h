#pragma once

namespace Domain
{

/// Interface for models that can be sampled/refreshed periodically.
class ISamplable
{
  public:
    virtual ~ISamplable() = default;

    /// Perform one sampling iteration (should be thread-safe).
    virtual void sample() = 0;
};

} // namespace Domain
