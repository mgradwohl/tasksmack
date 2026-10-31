#pragma once

// The layer behind TASKSMACK_SCREENSHOT (#2827), pushed last and only when the hook is set, so an
// ordinary launch pays nothing and its onPostRender() runs after UILayer has drawn the frame and
// before the frame is presented. It waits until `after` seconds have passed and the shell's startup
// selections (TASKSMACK_TAB, TASKSMACK_SELECT_PID/_NAME with TASKSMACK_DETAILS_TAB) have taken
// effect, gives ImGui SETTLE_FRAMES more frames to show what they selected, then captures the window
// (Core::FramebufferCapture, where the OpenGL stays) and writes the PNG, or the numbered sequence of
// them at `fps`, and asks the window to close: the same close request as the title bar's Close
// button, so config.toml is saved and the samplers stop as on any exit. main.cpp turns a run that
// wrote no screenshot into a non-zero exit code (outcome()).

#include "App/ScreenshotOverride.h"
#include "Core/Layer.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace App
{

class ShellLayer;

class ScreenshotLayer : public Core::Layer
{
  public:
    enum class Outcome : std::uint8_t
    {
        Pending, ///< Not captured yet
        Written, ///< Every frame written; the close was requested
        Failed,  ///< A read or write failed, or the app closed first; the close was requested
    };

    /// Rendered frames the capture waits after the startup selections settle: a tab ImGui selects a
    /// frame late, and the Process Details tab a selection opens after that, have drawn by then.
    static constexpr int SETTLE_FRAMES = 5;

    /// The frame rate asked for while a capture is pending, until a sequence's own rate takes over: the
    /// 20 fps idle rate, which a static tab no longer gets on its own (#3156).
    static constexpr double PENDING_FPS = 20.0;

    /// @p shell: the shell whose startup selections the capture waits for; nullptr when there is none.
    /// Non-owning: the application's layer stack holds both for the application's lifetime.
    ScreenshotLayer(ScreenshotOverride::Request request, const ShellLayer* shell);
    ~ScreenshotLayer() override = default;

    ScreenshotLayer(const ScreenshotLayer&) = delete;
    ScreenshotLayer& operator=(const ScreenshotLayer&) = delete;
    ScreenshotLayer(ScreenshotLayer&&) = delete;
    ScreenshotLayer& operator=(ScreenshotLayer&&) = delete;

    void onAttach() override;
    void onRender() override;
    void onPostRender() override;
    void onDetach() override;

    [[nodiscard]] Outcome outcome() const noexcept
    {
        return m_Outcome;
    }

    [[nodiscard]] int framesWritten() const noexcept
    {
        return m_FramesWritten;
    }

    /// The drawable size the last frame was captured at, in pixels; {0, 0} before any.
    [[nodiscard]] std::pair<int, int> capturedSize() const noexcept
    {
        return m_CapturedSize;
    }

  private:
    /// Reads and writes one frame (the file for frame m_FramesWritten + 1); false, logged, on failure.
    [[nodiscard]] bool captureFrame();
    /// Records the outcome and asks the window to close.
    void finish(Outcome outcome);

    ScreenshotOverride::Request m_Request;
    const ShellLayer* m_Shell = nullptr;
    Outcome m_Outcome = Outcome::Pending;
    double m_StartTime = 0.0;        ///< App clock at attach; `after` counts from it
    bool m_Settled = false;          ///< `after` passed and no startup selection pending
    int m_SettledFrames = 0;         ///< Frames rendered since then, up to SETTLE_FRAMES
    double m_FirstCaptureTime = 0.0; ///< The sequence's schedule starts here
    int m_FramesWritten = 0;
    std::pair<int, int> m_CapturedSize{0, 0};
    std::vector<std::uint8_t> m_Pixels; ///< Reused by every capture of a sequence
};

} // namespace App
