#pragma once

// Lets a panel stop a model being sampled while its tab is hidden without stopping the
// BackgroundSampler that drives it (#800). Stopping the sampler joins its thread, so hiding the tab
// in the middle of a slow sample would stall the UI thread until the sample finished; closing the
// gate returns at once, and the thread is joined only when the panel detaches, as the other panels'
// samplers are.

#include "Domain/ISamplable.h"

#include <atomic>
#include <memory>
#include <utility>

namespace App
{

/// An ISamplable that forwards sample() to its target only while open. Thread-safe: the UI thread
/// opens and closes it while the sampler thread samples through it. A sample already running when
/// the gate closes finishes on the sampler thread.
class SamplingGate final : public Domain::ISamplable
{
  public:
    explicit SamplingGate(std::weak_ptr<Domain::ISamplable> target) : m_Target(std::move(target))
    {}

    void setOpen(bool open) noexcept
    {
        m_Open.store(open, std::memory_order_release);
    }

    [[nodiscard]] bool isOpen() const noexcept
    {
        return m_Open.load(std::memory_order_acquire);
    }

    void sample() override
    {
        if (!isOpen())
        {
            return;
        }
        if (const auto target = m_Target.lock())
        {
            target->sample();
        }
    }

  private:
    std::weak_ptr<Domain::ISamplable> m_Target;
    std::atomic<bool> m_Open{false};
};

} // namespace App
