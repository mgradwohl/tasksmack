#pragma once

// Recovering from a GPU reset (#3484). When the GPU stops answering for about two seconds, Windows'
// timeout detection and recovery (TDR) resets it: a LiveKernelEvent 141 (VIDEO_ENGINE_TIMEOUT_DETECTED)
// in the Application log. The Intel OpenGL driver keeps an ordinary context running after one, but
// degraded for as long as #3484's runs watched (minutes, and through a minimize and restore): every
// frame waits one or two vblanks in the driver, so idle frames cost 15-30 ms instead of about 1 ms,
// and timer queries never complete. #882's 3.4-3.6 s stalls during bursts of window resizes are such
// timeouts.
//
// So Core::Window asks for a context that reports a reset (GL_LOSE_CONTEXT_ON_RESET, ARB_robustness),
// and Application checks glGetGraphicsResetStatus() once per frame. A reset seen there is handled by
// GraphicsResetMonitor below: it waits until the driver says the reset is over (the robustness spec's
// rule before making a new context), or GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS at the most, then asks
// for one recreation. GraphicsContextRecovery then runs it: the layers drop what lived in the old
// context (Layer::onGraphicsContextLost()), Window replaces the context and, only once that worked,
// graphicsContextGeneration() advances and the layers rebuild (Layer::onGraphicsContextRestored()).
// A failed replacement suspends drawing and is retried a few times before the app gives up and closes.
//
// Pure, so the decision is unit-tested without a GPU (tests/Core/test_GraphicsReset.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace Core
{

/// glGetGraphicsResetStatus()'s results (GL 4.5 / ARB_robustness / KHR_robustness share the values),
/// kept here so this header needs no OpenGL loader.
inline constexpr std::uint32_t GL_RESET_STATUS_NO_ERROR = 0;               // GL_NO_ERROR
inline constexpr std::uint32_t GL_RESET_STATUS_GUILTY = 0x8253;            // GL_GUILTY_CONTEXT_RESET
inline constexpr std::uint32_t GL_RESET_STATUS_INNOCENT = 0x8254;          // GL_INNOCENT_CONTEXT_RESET
inline constexpr std::uint32_t GL_RESET_STATUS_UNKNOWN = 0x8255;           // GL_UNKNOWN_CONTEXT_RESET
inline constexpr std::uint32_t GL_LOSE_CONTEXT_ON_RESET_STRATEGY = 0x8252; // GL_LOSE_CONTEXT_ON_RESET

/// The longest GraphicsResetMonitor waits for the driver to report a reset over before it recreates
/// the context anyway: a driver that keeps reporting the lost context must not leave the app on it.
inline constexpr double GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS = 2.0;

/// A glGetGraphicsResetStatus() result, for the log: "guilty", "innocent", "unknown", or "none".
[[nodiscard]] constexpr std::string_view graphicsResetStatusName(const std::uint32_t status) noexcept
{
    switch (status)
    {
    case GL_RESET_STATUS_NO_ERROR:
        return "none";
    case GL_RESET_STATUS_GUILTY:
        return "guilty";
    case GL_RESET_STATUS_INNOCENT:
        return "innocent";
    default:
        return "unknown"; // GL_UNKNOWN_CONTEXT_RESET, or a value the spec doesn't name
    }
}

/// What the frame loop does after one glGetGraphicsResetStatus() check.
enum class GraphicsResetAction : std::uint8_t
{
    None,     ///< No reset: carry on
    Wait,     ///< A reset is in progress: keep checking, don't make a context yet
    Recreate, ///< The reset is over (or took too long): replace the context now, once
};

/// Turns the per-frame glGetGraphicsResetStatus() results into one recreation per reset.
class GraphicsResetMonitor
{
  public:
    /// The action for @p status, read at @p nowSeconds (any steady clock).
    [[nodiscard]] GraphicsResetAction observe(const std::uint32_t status, const double nowSeconds) noexcept
    {
        if (!m_Lost)
        {
            if (status == GL_RESET_STATUS_NO_ERROR)
            {
                return GraphicsResetAction::None;
            }
            m_Lost = true;
            m_LostAt = nowSeconds;
            m_LostStatus = status;
            ++m_Resets;
            return GraphicsResetAction::Wait;
        }
        // Still reported and not yet too long: the reset may still be in progress.
        if (status != GL_RESET_STATUS_NO_ERROR && (nowSeconds - m_LostAt) < GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS)
        {
            return GraphicsResetAction::Wait;
        }
        m_Lost = false;
        return GraphicsResetAction::Recreate;
    }

    /// The status the last reset was first reported with (graphicsResetStatusName() names it).
    [[nodiscard]] std::uint32_t lostStatus() const noexcept
    {
        return m_LostStatus;
    }

    /// How many resets observe() has seen begin.
    [[nodiscard]] std::uint64_t resets() const noexcept
    {
        return m_Resets;
    }

  private:
    double m_LostAt = 0.0;
    std::uint64_t m_Resets = 0;
    std::uint32_t m_LostStatus = GL_RESET_STATUS_NO_ERROR;
    bool m_Lost = false;
};

/// The steps of replacing the context, as GraphicsContextRecovery runs them; Application binds them to
/// the layers and Window, the tests to fakes.
struct GraphicsContextRecoverySteps
{
    std::function<void()> loseLayers;        ///< Layer::onGraphicsContextLost(), top of the stack first
    std::function<bool()> recreateContext;   ///< Window::recreateGLContext(): false when it failed
    std::function<void()> advanceGeneration; ///< advanceGraphicsContextGeneration()
    std::function<void()> restoreLayers;     ///< swap interval, then Layer::onGraphicsContextRestored()
};

/// Where the renderer stands after a GPU reset.
enum class GraphicsRecoveryState : std::uint8_t
{
    Rendering, ///< A working context: frames draw
    Suspended, ///< The layers dropped their GL objects and no new context could be made yet: no frame draws
    GaveUp,    ///< Every attempt failed: the app closes, with the error logged
};

/// Replaces the context after a reset, and decides what happens when that fails (#3484). The layers
/// are restored -- and the context generation advanced -- only after a replacement succeeds: a
/// layer rebuilt on a failed or missing context would make GL calls with no renderer behind them.
/// A failure suspends drawing (Application skips its frames, still pumping events) and retries after
/// each of GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS, since a driver still recovering from the reset
/// can refuse a context for a moment; after the last one it gives up and Application closes, rather
/// than run on without a renderer. UI thread.
class GraphicsContextRecovery
{
  public:
    /// Waits before each retry after a failed replacement, seconds: about 15 s in all.
    static constexpr std::array<double, 5> GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS{0.5, 1.0, 2.0, 4.0, 8.0};

    /// A reset is over (GraphicsResetMonitor said Recreate): drop the layers' GL objects and replace
    /// the context, at @p nowSeconds.
    GraphicsRecoveryState replace(const double nowSeconds, const GraphicsContextRecoverySteps& steps)
    {
        steps.loseLayers();
        m_Attempts = 0;
        return attempt(nowSeconds, steps);
    }

    /// While Suspended, the next attempt once its delay has passed; otherwise nothing. The state after.
    GraphicsRecoveryState retryIfDue(const double nowSeconds, const GraphicsContextRecoverySteps& steps)
    {
        if (m_State != GraphicsRecoveryState::Suspended || nowSeconds < m_NextAttemptAt)
        {
            return m_State;
        }
        return attempt(nowSeconds, steps);
    }

    [[nodiscard]] GraphicsRecoveryState state() const noexcept
    {
        return m_State;
    }

    /// Replacements tried since the last reset began.
    [[nodiscard]] std::size_t attempts() const noexcept
    {
        return m_Attempts;
    }

  private:
    GraphicsRecoveryState attempt(const double nowSeconds, const GraphicsContextRecoverySteps& steps)
    {
        ++m_Attempts;
        if (steps.recreateContext())
        {
            steps.advanceGeneration();
            steps.restoreLayers();
            m_State = GraphicsRecoveryState::Rendering;
        }
        else if (m_Attempts > GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS.size())
        {
            m_State = GraphicsRecoveryState::GaveUp;
        }
        else
        {
            m_State = GraphicsRecoveryState::Suspended;
            m_NextAttemptAt = nowSeconds + GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS.at(m_Attempts - 1);
        }
        return m_State;
    }

    double m_NextAttemptAt = 0.0;
    std::size_t m_Attempts = 0;
    GraphicsRecoveryState m_State = GraphicsRecoveryState::Rendering;
};

/// Advanced by Application each time it replaces the OpenGL context after a reset: a cache of GL
/// object names (UI::IconTextureCache, UI::Texture) compares it with the generation it made them in, and forgets
/// them -- without deleting, as they belonged to the old context -- when it has moved on. UI thread.
[[nodiscard]] inline std::uint64_t& graphicsContextGenerationStorage() noexcept
{
    static std::uint64_t generation = 0;
    return generation;
}

[[nodiscard]] inline std::uint64_t graphicsContextGeneration() noexcept
{
    return graphicsContextGenerationStorage();
}

inline void advanceGraphicsContextGeneration() noexcept
{
    ++graphicsContextGenerationStorage();
}

} // namespace Core
