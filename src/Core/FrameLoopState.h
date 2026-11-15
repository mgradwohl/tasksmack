#pragma once

// Application::run()'s loop state and the phases of one loop iteration that work on it (#3791).
//
// The loop runs three state machines that share its state: the resize present hold (#882), drawing
// suspended after a GPU reset (#3484) and the idle wait's timer (#3712). Their order was readable only
// in one loop of several hundred lines. Here the state is one struct, and each phase is a method that
// a test can call on its own, with a fake IFrameLoopHost for what the phase calls out to: the clock,
// the paced delay, the event queue and its wait, the layers and the window, the frame's draw and the
// GPU recovery. iterate() runs one whole iteration: the drain, the bookkeeping after it, the three
// frame phases and the end of the iteration, in that order.
//
// The decisions stay pure, in FramePacing.h; the phases only put them in order and act on them.
// Loop thread only.

#include "Core/FramePacing.h"
#include "Core/ResizePerfTrace.h"
#include "Core/WindowConstants.h"
#include "Core/WindowVisibility.h"

#include <SDL3/SDL_events.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace Core
{

/// The timings Application::renderFrame() reports for one traced frame, in milliseconds.
struct FrameTimes
{
    double updateMs = 0.0;
    double renderMs = 0.0;
    double postRenderMs = 0.0;
    double swapMs = 0.0;
};

/// What the loop's phases call out to: Application in the app, a fake in tests.
class IFrameLoopHost
{
  public:
    IFrameLoopHost() = default;
    virtual ~IFrameLoopHost() = default;
    IFrameLoopHost(const IFrameLoopHost&) = delete;
    IFrameLoopHost& operator=(const IFrameLoopHost&) = delete;
    IFrameLoopHost(IFrameLoopHost&&) = delete;
    IFrameLoopHost& operator=(IFrameLoopHost&&) = delete;

    /// The frame clock, in seconds (Application::getTime()).
    [[nodiscard]] virtual double now() = 0;
    /// A paced wait of @p nanoseconds (SDL_DelayPrecise()).
    virtual void delayPreciseNs(std::uint64_t nanoseconds) = 0;
    /// Draws one frame with this frame's deltaTime (Application::renderFrame()). @p times is null unless
    /// the frame is traced. False when nothing was drawn: drawing is suspended after a GPU reset (#3484).
    virtual bool renderFrame(bool resizeTriggeredFrame, FrameTimes* times) = 0;
    /// One event wait of up to @p timeoutMs (FramePacing::waitForIdleEvent() over SDL's wait and poll),
    /// ended on time on Windows by IdleWaitTimer (#3712). The event that ends it is left in @p event.
    virtual FramePacing::IdleWaitOutcome waitForEvent(SDL_Event& event, int timeoutMs) = 0;
    /// @p waitMs, cut short to the next retry of a failed context replacement (GraphicsContextRecovery::boundWaitMs(), #3484).
    [[nodiscard]] virtual int boundWaitMs(int waitMs, double nowSeconds) = 0;
    /// The layers' onHiddenUpdate(), for an iteration of a hidden window that draws no frames (#3156).
    virtual void hiddenUpdate() = 0;
    /// How the idle wait just ended: Application's idle-wait test seam (#1446, #1450).
    virtual void noteIdleWait(FramePacing::IdleWaitOutcome outcome) = 0;

    // ---- The drain ----

    /// The drain's clock (std::chrono::steady_clock): its budget (P0) and the P3 render skip (#1410).
    [[nodiscard]] virtual std::chrono::steady_clock::time_point drainClock() = 0;
    /// Takes the next queued event into @p event (SDL_PollEvent()); false when the queue is empty.
    virtual bool pollEvent(SDL_Event& event) = 0;
    /// Takes @p event if it is a publication wake (Domain::PublicationWake, #3156), which no layer
    /// handles: true when it was one.
    virtual bool takePublicationWake(const SDL_Event& event) = 0;
    /// Hands @p event to the layers' onSDLEvent(), for ImGui and the layers' own input handling.
    virtual void dispatchToLayers(SDL_Event& event) = 0;
    /// A close request from an event: raised as a WindowCloseEvent, which stops the app unless a layer
    /// handles it (contract in WindowEvents.h).
    virtual void closeOnRequest() = 0;
    /// Stops the app: SIGINT/SIGTERM and OS logout or shutdown, which no layer can veto (#1150).
    virtual void quit() = 0;
    /// The window's framebuffer size, in pixels.
    [[nodiscard]] virtual std::pair<int, int> windowSizeInPixels() = 0;
    /// Raises a WindowResizedEvent for a new framebuffer size.
    virtual void raiseResized(std::pair<int, int> pixelSize) = 0;
    /// The window moved, resized or changed scale: Window::handleGeometryChanged() (#1250).
    virtual void windowGeometryChanged() = 0;
    /// The window is on another display, or its mode changed: re-read the refresh rate (#1126).
    virtual void displayChanged() = 0;
    /// An OS maximize of the borderless window (Window::handleSystemMaximized(), #1208, #1250).
    virtual void systemMaximized() = 0;
    /// A window-manager or compositor restore of an OS maximize (#1250).
    virtual void systemRestored() = 0;
    /// A minimize: Win+Down on the client-side maximized window restores it instead (#1279).
    virtual void shellMinimized() = 0;
    /// When the move/resize grace ends (Application::isInteractionRedrawActive()).
    [[nodiscard]] virtual double interactionRedrawUntil() = 0;
    virtual void setInteractionRedrawUntil(double seconds) = 0;

    // ---- Between the drain and the frames ----

    /// Whether a layer moved or resized the window during the previous frame
    /// (Application::signalWindowGeometryChanged()), clearing it for this one.
    [[nodiscard]] virtual bool takeGeometryChanged() = 0;
    /// The display refresh rate frames are paced against, in Hz (#1126).
    [[nodiscard]] virtual double displayRefreshHz() = 0;
    /// Whether the app asked for vsync (ApplicationSpecification::VSync).
    [[nodiscard]] virtual bool vsyncRequested() = 0;
    /// Whether vsync is off for a move/resize interaction now.
    [[nodiscard]] virtual bool vsyncDisabledForInteraction() = 0;
    /// Turns vsync off for an interaction, or back on after it (P1). While drawing is suspended after a
    /// GPU reset (#3484) only the wanted state is kept, and the context's replacement applies it.
    virtual void setVsyncDisabledForInteraction(bool disabled) = 0;
    /// A close the window asked for itself (Window::requestClose(): the title bar's Close, the system
    /// menu, #1077), with the same WindowCloseEvent veto: true when it stopped the app.
    virtual bool closeOnWindowRequest() = 0;
    [[nodiscard]] virtual bool isMinimized() = 0;
    /// Once per iteration, drawing or not: the GPU reset check, or a failed replacement's retry (#3484).
    virtual void pollGraphicsRecovery() = 0;
    /// Window::pollVisibility(), then the window's hidden state (#1125, #3152).
    [[nodiscard]] virtual WindowVisibility::HiddenState pollHiddenState() = 0;
    /// Pauses the publication wakes while nothing is drawn (#3156): the event that shows the window
    /// draws the newest data.
    virtual void pausePublicationWakes(bool paused) = 0;
};

/// Application::run()'s state between and within loop iterations (#3791).
struct FrameLoopState
{
    // When no SDL events arrive, render the next frame this long after the previous one started
    // (FramePacing::computeIdleWaitMs, #1276). This holds the idle render rate at 20 fps, reducing CPU usage when the display
    // hasn't changed. Mouse movement and keyboard events wake the sleep immediately,
    // so interactive frame rate is unaffected.
    static constexpr int IDLE_FRAME_SLEEP_MS = 50;
    // The frame rate that idle sleep gives without input. Motion that needs no more than this is left
    // to the idle path rather than paced (FramePacing::computeAnimationRate, #1125).
    static constexpr double IDLE_FRAME_RATE = 1000.0 / IDLE_FRAME_SLEEP_MS;
    // The fastest the loop paces frames, whatever asks: a visible chart or NowBar moving fast enough
    // (Core::AnimationRequest, #1037/#1125) or input (#1153). A whole number of display refreshes never
    // slower than this (FramePacing::vblanksPerFrame, #1126): 60 fps at 60/120 Hz, 75 at 75 Hz, 72 at
    // 144 Hz, 82.5 at 165 Hz. A move/resize interaction is capped at the display rate instead.
    static constexpr double MAX_FRAME_RATE = 60.0;
    // The hidden rate, ~5 fps, for the hidden window that still draws (FramePacing::computeHiddenWait):
    // before the first frame, and on a native Wayland minimize. Any other hidden window draws no frames
    // at all (#3156). Any event (e.g. SDL_EVENT_WINDOW_RESTORED) wakes immediately, so restore latency
    // is unaffected.
    static constexpr int MINIMIZED_FRAME_SLEEP_MS = 200;
    // The slow floor of a static frame (#3156 stage 2): see WindowConstants.h.
    static constexpr int STATIC_FRAME_FLOOR_MS = static_cast<int>(STATIC_FRAME_FLOOR_INTERVAL.count());
    static_assert(STATIC_FRAME_FLOOR_MS > IDLE_FRAME_SLEEP_MS, "the floor is slower than the idle rate it replaces");
    // How long the no-frames wait of a hidden window lasts at most (#3156): see WindowConstants.h.
    static constexpr int HIDDEN_WAIT_TIMEOUT_MS = static_cast<int>(WINDOW_HIDDEN_WAIT_TIMEOUT.count());
    static constexpr int CLOAKED_POLL_INTERVAL_MS = static_cast<int>(WINDOW_CLOAKED_POLL_INTERVAL.count());

    // During interactive move/resize, event delivery can be bursty depending on
    // compositor/window-manager behavior. Keep redraw active for a short grace
    // window after each relevant window event so the framebuffer stays responsive
    // without forcing continuous high-rate rendering when idle.
    static constexpr double INTERACTION_REDRAW_GRACE_SECONDS = 0.35;
    // How long after an input event (a wheel, a button, a key, text: WindowEventRouting::isInputEvent) the
    // regular frame stays paced at MAX_FRAME_RATE instead of dropping to the idle wait (#843). Dear ImGui
    // finishes reacting to input over the next frame or two (a popup or combo list is drawn the frame after
    // it opens, a clicked tab's content the frame after the click, hover and tooltip delays tick per frame),
    // and at the 20 fps idle rate each of those frames came 50 ms apart. Long enough to cover ImGui's
    // hover delays and the gap between keystrokes while typing; short enough that a single click costs
    // about a dozen extra frames. Does not touch sampling, the hidden-window throttle (#1125) or the
    // move/resize vsync transition, which stays tied to resize/move events.
    static constexpr double INPUT_REDRAW_GRACE_SECONDS = 0.2;

    // P0: Break the event drain loop if wall-clock drain exceeds this threshold.
    // On Wayland, individual SDL_PollEvent calls can stall on compositor protocol
    // (xdg_surface configure handshake). Capping drain limits per-frame stall time.
    static constexpr double DRAIN_BUDGET_MS = 8.0;
    // P3: If total drain time exceeds a full 60 fps frame budget, skip rendering
    // this frame. The event queue is fully processed; rendering catches up next frame.
    static constexpr double DRAIN_SKIP_RENDER_MS = 16.0;

    static constexpr double RESIZE_PERF_TRACE_LOG_INTERVAL_SECONDS = 0.5;
    // Idle/steady-state frames are logged on a much longer cadence than interaction frames: an
    // interaction is a short, bounded burst where frequent logging is useful, but idle frames run
    // indefinitely while the app just sits open, so 0.5s would spam the log forever (perf-plan #843
    // phase 0 — idle-time performance is priority 1, but that doesn't mean logging it every tick).
    static constexpr double IDLE_PERF_TRACE_LOG_INTERVAL_SECONDS = 5.0;

    /// What drain() took off the queue and handled.
    struct DrainResult
    {
        FramePacing::EventDrainResult events{};
        std::uint32_t resizeEventCount = 0; ///< real new sizes and moves: they start the move/resize grace
        /// Wheel, button, key and text events: they start the input redraw grace (#843).
        std::uint32_t inputEventCount = 0;
        bool needsResizeRedraw = false; ///< a WindowResizedEvent was raised
        double totalDrainMs = 0.0;
    };

    /// What afterDrain() decided for the rest of the iteration.
    struct IterationState
    {
        FramePacing::HiddenWait hiddenWait{};
        /// A close the window asked for was accepted (closeOnWindowRequest()): the loop ends here.
        bool stopped = false;
        bool inputGraceActive = false;
        bool forceInteractionRedraw = false; ///< within the move/resize grace
        bool isInteracting = false;
        bool tracingInteraction = false;
        bool skipRenderThisFrame = false; ///< P3
        bool isHidden = false;
    };

    /// What endIteration() reads from this iteration.
    struct IterationEnd
    {
        bool isInteracting = false;
        bool tracingInteraction = false;
        bool skipRenderThisFrame = false;
        bool drewFrame = false; ///< a renderFrame() this iteration drew (not while drawing is suspended, #3484)
        bool traced = false;
    };

    /// What interactionFrame() reads from this iteration's drain and hidden-state check.
    struct InteractionFrameInput
    {
        bool drawsNoFrames = false;          ///< FramePacing::HiddenWait::drawsNoFrames
        bool needsResizeRedraw = false;      ///< the drain raised a WindowResizedEvent
        bool forceInteractionRedraw = false; ///< within the move/resize grace
        bool hadEvents = false;
        bool isHidden = false;
        bool skipRenderThisFrame = false; ///< P3: the drain ran over a frame's budget
        bool traced = false;              ///< resize perf tracing is on this iteration
        double displayRefreshHz = 0.0;
    };

    /// What interactionFrame() decided and did.
    struct InteractionFrameResult
    {
        bool holdPresent = false;              ///< a held present (#882): no frame this iteration
        bool presentOwedFrame = false;         ///< the hold ended and the frame it owed is due
        bool didImmediateResizeRedraw = false; ///< a move/resize frame was attempted
        bool drewFrame = false;                ///< ... and drew
    };

    /// What waits() reads from this iteration.
    struct WaitInput
    {
        FramePacing::HiddenWait hiddenWait{};
        bool hadEvents = false;
        bool inputGraceActive = false; ///< within the input redraw grace (#843)
        bool isHidden = false;
        bool holdPresent = false;
        bool didImmediateResizeRedraw = false;
        double interactionRedrawUntil = 0.0; ///< when the move/resize grace ends
        bool traced = false;
        double displayRefreshHz = 0.0;
    };

    /// Which of waits()'s branches ran.
    enum class Wait : std::uint8_t
    {
        None,       ///< no wait: events were drained, or a grace keeps the frames coming
        Hidden,     ///< a hidden window that draws no frames: its wait (unless events were drained) and onHiddenUpdate()
        ResizeHold, ///< a held present's wait (#882)
        PacedFrame, ///< the regular frame paced at a frame rate cap (it may need no delay)
        Idle,       ///< the idle wait
    };

    /// What waits() did.
    struct WaitResult
    {
        Wait wait = Wait::None;
        /// How the idle wait ended, when it ran (Wait::Idle): Application's idle-wait test seam (#1446).
        std::optional<FramePacing::IdleWaitOutcome> idleWaitOutcome;
    };

    /// What regularFrame() reads from this iteration.
    struct RegularFrameInput
    {
        bool didImmediateResizeRedraw = false;
        bool skipRenderThisFrame = false;
        bool drawsNoFrames = false;
        bool holdPresent = false;
        bool traced = false;
    };

    /// One loop iteration: beginIteration(), drain(), afterDrain(), interactionFrame(), waits(),
    /// regularFrame() and endIteration(), in that order. False when a close the window asked for stopped
    /// the app (afterDrain()): that iteration ends after the bookkeeping, without a frame.
    bool iterate(IFrameLoopHost& host, bool traced);

    /// The start of an iteration: the previous traced loop's line is finished and this one timed
    /// (when @p traced), and the previous frame's geometry change is taken for this iteration's waits.
    void beginIteration(IFrameLoopHost& host, bool traced);

    /// Handles the queued events, the event that woke the previous wait first (#1409), until the queue
    /// is empty or the drain runs over DRAIN_BUDGET_MS (P0, #1410). A publication wake is only taken
    /// (#3156); every other event goes to the layers, and then to the window's own handling: a close,
    /// a new size (a WindowResizedEvent, and the move/resize grace), a move, a display change, an OS
    /// maximize or restore.
    DrainResult drain(IFrameLoopHost& host, bool traced);

    /// The bookkeeping between the drain and the frames: the input and move/resize graces, the vsync
    /// transition (P1), the trace's idle<->interaction transitions and this drain's batch, a close the
    /// window asked for, the P3 skip, the GPU recovery's poll and the hidden state (#3156).
    IterationState afterDrain(IFrameLoopHost& host, const DrainResult& drained, bool traced);

    /// The move/resize frame (#1153, #3511) and the resize present hold (#882): whether this iteration
    /// holds its present, owes a frame or pays one, and the frame itself, paced at the display rate.
    InteractionFrameResult interactionFrame(IFrameLoopHost& host, const InteractionFrameInput& input);

    /// The iteration's wait, if any: a hidden window's wait and onHiddenUpdate(), a held present's wait,
    /// the paced delay of an animation or input frame, or the idle wait (#1276, #3156). Every event wait
    /// is cut short to the next retry of a failed context replacement (#3484).
    WaitResult waits(IFrameLoopHost& host, const WaitInput& input);

    /// The regular frame, unless the iteration drew a move/resize frame, skipped its render, woke on an
    /// event (#1409), draws no frames while hidden (#3156) or holds its present (#882). True when it drew.
    bool regularFrame(IFrameLoopHost& host, const RegularFrameInput& input);

    /// The end of an iteration: the trace's periodic summary, and what the frames drawn ask of the next
    /// one (Core::AnimationRequest, #1037, #3156); the P3 skip count (#1410).
    void endIteration(IFrameLoopHost& host, const IterationEnd& input);

    /// Records the traced loop that started at loopStart and ends at @p end, and logs its phases when
    /// it ran long (ResizePerfLoop). Nothing before the first traced loop.
    void finishTracedLoop(std::uint64_t end);

    /// Waits up to @p timeoutMs for an event, timing the wait for the trace when @p traced. The event
    /// that ends the wait is kept in idleWakeEvent, for the next drain to dispatch first (#1409, #1450).
    FramePacing::IdleWaitOutcome waitForEvent(IFrameLoopHost& host, int timeoutMs, bool traced);

    /// Records the deliver-to-deliver interval of the frame just presented (#843 measurement kit).
    void recordDeliveredFrame();

    ResizePerfTraceStats resizeTraceStats;
    bool wasTracingInteraction = false;
    bool wasInteracting = false;
    double lastResizeTraceLogTime = 0.0;
    // Snapshot of whether TitleBarLayer changed window geometry (position/size) during
    // the PREVIOUS frame's onUpdate. Used to gate grace-period sleep: if no geometry
    // changed last frame, we allow the idle sleep even inside the interaction grace window,
    // preventing wasted renders when the window is stationary post-interaction.
    bool geometryChangedLastFrame = false;
    // The highest frame rate the previous frame's moving content asked for (Core::AnimationRequest,
    // 0 = nothing moved visibly), and when the last frame started: together they pace the next
    // frame (#1037, #1125).
    double requestedAnimationFps = 0.0;
    // The soonest frame the previous frame's content asked for, in seconds after it started
    // (Core::AnimationRequest::requestFrameWithin(), infinity = none, #3156).
    double requestedFrameWithinSeconds = std::numeric_limits<double>::infinity();
    double lastFrameStart = 0.0;
    // The event that woke the idle wait, taken off the queue by SDL_WaitEventTimeout(): the next
    // iteration's drain dispatches it before anything else, and before that iteration renders (#1409).
    std::optional<SDL_Event> idleWakeEvent;
    // Whether a frame has been drawn yet: until then a hidden window draws at the hidden rate, so the
    // first update publishes the loaded settings even in an app started minimized (#3156).
    bool drewFirstFrame = false;
    // P3 skips in a row, for the FramePacing::MAX_CONSECUTIVE_SKIPPED_RENDERS bound (#1410).
    std::uint32_t consecutiveSkippedRenders = 0;
    // When the input redraw grace ends (FramePacing::computeInputRedrawUntil, #843): until then the
    // regular frame is paced as an input frame, so the frames after a click or a key come at the full
    // rate, not the idle one. 0 = no input yet.
    double inputRedrawUntil = 0.0;
    std::uint64_t loopStart = 0;
    ResizePerfLoopTiming loopTiming;

    // The framebuffer size of the last WindowResizedEvent, so an SDL_EVENT_WINDOW_EXPOSED can tell
    // a repaint (same size) from a resize that surfaced only as an expose (#1154).
    std::pair<int, int> lastResizePixelSize;

    // The present bound during an interactive resize (#882, FramePacing::computeResizePresent): when the
    // framebuffer size last changed, and whether a held frame is still owed. Windows only: there #3511's
    // supervised run still hit a 2.5 s present inside the driver after a few seconds of rapid resizing.
    // A Wayland compositor paces a resize to the client's commits itself, so holding them there would
    // only slow the resize, and no X11 stall has been reported.
    double lastSizeChangeTime = -std::numeric_limits<double>::infinity();
    bool resizePresentOwed = false;
    double resizePresentMinInterval = 0.0;

  private:
    /// Handles the next event, false once the queue is empty: drain()'s step.
    bool pollAndDispatch(IFrameLoopHost& host, DrainResult& drained);
    /// Raises a WindowResizedEvent for a new framebuffer size and starts the move/resize grace that
    /// keeps a live resize responsive.
    void handleResize(IFrameLoopHost& host, std::pair<int, int> pixelSize, DrainResult& drained);
    /// A paced delay of @p seconds, timed for the trace when @p traced.
    void pacedDelay(IFrameLoopHost& host, double seconds, bool traced);
    /// Starts and draws one frame, and records it for the trace when @p traced and it drew.
    bool drawFrame(IFrameLoopHost& host, bool resizeTriggeredFrame, bool traced);
};

} // namespace Core
