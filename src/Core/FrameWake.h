#pragma once

// The frame loop's publication wake (#3156): at most one wake event pending at a time.
//
// A sampler or background task that publishes calls Domain::PublicationWake::notify(); the handler
// Application::run() installs pushes one SDL user event through this coalescer. While a wake is
// pending (pushed, and not yet taken off the queue by the loop's drain), further notifications push
// nothing: a fast sampler, or several publishing at once, can't flood the event queue, and every one
// of them is shown by the frame that the pending wake brings, which reads after the drain.
//
// The order that makes a coalesced notification safe: the loop calls taken() when its drain takes the
// wake event, and only then draws. A publisher commits first and notifies after. A notification
// coalesced into a pending wake therefore committed before the drain took that wake, and the frame after
// the drain reads it. One that comes after taken() pushes a new wake. Should a notification ever be
// lost all the same, the static floor (STATIC_FRAME_FLOOR_INTERVAL) still draws it, a little late.
//
// Paused while the window is hidden and draws no frames (#3156 stage 1): a hidden window has nothing
// to redraw, and the event that shows it draws the newest data anyway.
//
// Free of SDL: the push is a callable, so this is tested on its own (tests/Core/test_Application.cpp).

#include <atomic>

namespace Core
{

class WakeCoalescer
{
  public:
    /// From any thread: pushes the wake with @p push (a callable returning whether it was queued),
    /// unless one is already pending or the loop paused wakes. A push that fails (a full queue) leaves
    /// no wake pending, so the next notification tries again.
    /// @return Whether this call queued a wake.
    template<typename Push> bool notify(Push push) noexcept
    {
        if (m_Paused.load())
        {
            return false;
        }
        if (m_Pending.exchange(true))
        {
            return false; // coalesced into the pending wake
        }
        if (!push())
        {
            m_Pending.store(false);
            return false;
        }
        return true;
    }

    /// The loop's drain took the wake event off the queue: the next notification pushes a new one.
    void taken() noexcept
    {
        m_Pending.store(false);
    }

    /// Whether a wake is pushed and not yet taken.
    [[nodiscard]] bool pending() const noexcept
    {
        return m_Pending.load();
    }

    /// While @p paused, notifications push nothing (the hidden window draws no frames).
    void setPaused(bool paused) noexcept
    {
        m_Paused.store(paused);
    }

    [[nodiscard]] bool paused() const noexcept
    {
        return m_Paused.load();
    }

    /// Back to the start: nothing pending, not paused. For a new run() of the loop.
    void reset() noexcept
    {
        m_Pending.store(false);
        m_Paused.store(false);
    }

  private:
    // Sequentially consistent throughout: a publisher's commit, then its notify(), against the loop's
    // taken(), then its read. These run a few times a second, so the strongest order costs nothing.
    std::atomic<bool> m_Pending{false};
    std::atomic<bool> m_Paused{false};
};

} // namespace Core
