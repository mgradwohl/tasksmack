#pragma once

// "New data is published": the wake a publisher sends the frame loop after it commits (#3156).
//
// A static tab draws only when something on it changes. The frame loop waits for input, for its slow
// floor, or for this: every BackgroundSampler pass that sampled calls notify() once it has committed,
// as does every App background task once its result is ready, so the frame that follows shows it.
// Publishers call it in those two places, never per panel.
//
// Domain can't depend on Core, so the loop installs the handler (Core::Application::run()): it pushes
// one coalesced SDL event, from any thread. With no handler installed (tests, a headless app, before
// run() or after it), notify() does nothing. Thread-safe and lock-free: the handler is a function
// pointer in an atomic.

#include <atomic>

namespace Domain::PublicationWake
{

/// What notify() calls. It must be safe to call from any thread, and must not throw.
using Handler = void (*)() noexcept;

namespace Detail
{
[[nodiscard]] inline auto handler() noexcept -> std::atomic<Handler>&
{
    static std::atomic<Handler> current{nullptr};
    return current;
}
} // namespace Detail

/// Installs @p handler (nullptr removes it), returning the one it replaces. A notify() already running
/// may still call the old one, so a handler stays safe to call after it is removed.
inline auto setHandler(Handler handler) noexcept -> Handler
{
    return Detail::handler().exchange(handler, std::memory_order_acq_rel);
}

/// Tells the frame loop that new data is published. Call after the commit, never before: the frame the
/// wake brings reads what was published by then.
inline void notify() noexcept
{
    if (const Handler handler = Detail::handler().load(std::memory_order_acquire); handler != nullptr)
    {
        handler();
    }
}

} // namespace Domain::PublicationWake
