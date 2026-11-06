#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace Core
{

/// The swap intervals Window::setVSync() asks SDL_GL_SetSwapInterval() for.
inline constexpr int SWAP_INTERVAL_IMMEDIATE = 0; ///< no vsync
inline constexpr int SWAP_INTERVAL_VSYNC = 1;     ///< regular vsync
inline constexpr int SWAP_INTERVAL_ADAPTIVE = -1; ///< adaptive vsync (WGL/GLX_EXT_swap_control_tear)

/// What Window::setVSync() last set on the current OpenGL context, so it asks the driver only for a
/// change (#3511). Turning vsync on prefers adaptive vsync and falls back to regular vsync; a context
/// that refuses adaptive vsync is remembered, so it is asked once, not at the end of every resize
/// interaction (Intel's Windows driver refuses it, and #3511's resize bursts asked about 64 times a
/// run). Everything is per context: a different context (a replacement after a GPU reset, #3484)
/// starts over, since the swap interval belongs to the context.
///
/// Pure: the caller passes the context's identity and a setter, so the decisions are unit-tested
/// without a GL context. UI thread only, like every GL call.
class SwapIntervalCache
{
  public:
    /// The intervals to try, in order, until one is accepted: none when the context already has what
    /// was asked for.
    struct Attempts
    {
        std::array<int, 2> intervals{};
        std::size_t count = 0;

        [[nodiscard]] std::span<const int> view() const noexcept
        {
            return std::span<const int>{intervals}.first(count);
        }
    };

    /// What apply() did.
    struct Outcome
    {
        std::size_t calls = 0;        ///< SDL_GL_SetSwapInterval() calls made; 0 = already set
        bool accepted = false;        ///< one was accepted, or none was needed
        bool adaptiveRefused = false; ///< this call found adaptive vsync unsupported (logged once)
        std::optional<int> applied;   ///< the context's interval now, if known
    };

    /// The intervals to try to turn vsync on (@p enabled) or off on the context @p context.
    [[nodiscard]] Attempts plan(bool enabled, const void* context) noexcept
    {
        adopt(context);
        Attempts attempts;
        if (!enabled)
        {
            if (m_Applied != SWAP_INTERVAL_IMMEDIATE)
            {
                attempts.intervals[attempts.count++] = SWAP_INTERVAL_IMMEDIATE;
            }
            return attempts;
        }
        if (m_Applied == SWAP_INTERVAL_ADAPTIVE || m_Applied == SWAP_INTERVAL_VSYNC)
        {
            return attempts; // vsync is on already, adaptive or regular
        }
        if (!m_AdaptiveUnsupported)
        {
            attempts.intervals[attempts.count++] = SWAP_INTERVAL_ADAPTIVE;
        }
        attempts.intervals[attempts.count++] = SWAP_INTERVAL_VSYNC;
        return attempts;
    }

    /// The driver's answer to one SDL_GL_SetSwapInterval(@p interval) call on the context plan() saw.
    void record(int interval, bool accepted) noexcept
    {
        if (accepted)
        {
            m_Applied = interval;
            return;
        }
        if (interval == SWAP_INTERVAL_ADAPTIVE)
        {
            m_AdaptiveUnsupported = true; // the interval set before is still in force
            return;
        }
        m_Applied.reset(); // unknown now: the next request asks again
    }

    /// plan() and record() around @p set (bool(int interval), true when the driver accepted it): tries
    /// each planned interval until one is accepted.
    template<typename Set> Outcome apply(bool enabled, const void* context, Set set)
    {
        const bool adaptiveKnownUnsupported = m_Context == context && m_AdaptiveUnsupported;
        const Attempts attempts = plan(enabled, context);
        Outcome outcome;
        outcome.accepted = attempts.count == 0;
        for (const int interval : attempts.view())
        {
            ++outcome.calls;
            const bool accepted = set(interval);
            record(interval, accepted);
            if (accepted)
            {
                outcome.accepted = true;
                break;
            }
        }
        outcome.adaptiveRefused = !adaptiveKnownUnsupported && m_AdaptiveUnsupported;
        outcome.applied = m_Applied;
        return outcome;
    }

    /// The interval in force on the current context, if known.
    [[nodiscard]] std::optional<int> applied() const noexcept
    {
        return m_Applied;
    }

    /// Whether the current context refused adaptive vsync.
    [[nodiscard]] bool adaptiveUnsupported() const noexcept
    {
        return m_AdaptiveUnsupported;
    }

  private:
    void adopt(const void* context) noexcept
    {
        if (context != m_Context)
        {
            *this = {};
            m_Context = context;
        }
    }

    const void* m_Context = nullptr;
    std::optional<int> m_Applied;
    bool m_AdaptiveUnsupported = false;
};

} // namespace Core
