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
// for one recreation. Application then lets the layers drop what lived in the old context
// (Layer::onGraphicsContextLost()), has Window replace the context, advances graphicsContextGeneration()
// and lets the layers rebuild (Layer::onGraphicsContextRestored()).
//
// Pure, so the decision is unit-tested without a GPU (tests/Core/test_GraphicsReset.cpp).

#include <cstdint>
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

/// Advanced by Application each time it replaces the OpenGL context after a reset: a cache of GL
/// object names (UI::IconTextureCache) compares it with the generation it made them in, and forgets
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
