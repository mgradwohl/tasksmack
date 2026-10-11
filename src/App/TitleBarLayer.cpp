#include "TitleBarLayer.h"

#include "App/TitleBarGeometry.h"
#include "App/TitleBarView.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/EnvUtils.h"
#include "Core/Layer.h"
#include "Core/ResizePerfOperation.h"
#include "Core/Utf8Path.h"
#include "Core/VideoBackend.h"
#include "Core/WindowEvents.h"
#include "UI/AssetPath.h"
#include "UI/IconLoader.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <ratio>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace App
{

// The title bar is chrome and is sized from display density only -- never from the application's
// Font Size setting. See the note at the top of TitleBarGeometry.h for why, including the hit-test
// bug that a body-font-derived height caused.
auto TitleBarLayer::height() -> float
{
    // Specified in points and converted once against the display scale (see UILayer), so the bar is
    // a fixed physical size on any display. Read from Theme rather than recomputed so it is valid
    // outside an ImGui frame, which SDL's hit-test callback needs.
    return UI::Theme::get().titleBarHeightPx();
}

namespace
{
// The point-in-bounds test and resize-edge detection now live in TitleBarGeometry.h
// (computeIsPointInBounds, computeDetectResizeEdge) so they're directly unit-testable
// without linking this file - see #769. The resize-perf-tracing env-var check below now
// reuses Core::isEnvFlagEnabled() instead of its own duplicate case-insensitive parser.

// Shared resize border thickness -- must stay in sync between hit-test and cursor detection, so
// both ask here. Scaled with the display like the title bar itself; see
// computeResizeBorderThickness(). Valid outside an ImGui frame, which SDL's hit-test callback needs.
[[nodiscard]] auto resizeBorderThickness() -> float
{
    return computeResizeBorderThickness(UI::Theme::get().displayScale());
}

constexpr double RESIZE_SIZE_COMMIT_INTERVAL_SECONDS = 1.0 / 20.0;

// Conditionally time an operation and accumulate the duration into accumMs.
// When traceEnabled is false the call reduces to a branch and a direct callable invocation.
template<typename Fn> void timedOp(bool traceEnabled, double& accumMs, Fn&& fn)
{
    if (!traceEnabled)
    {
        std::forward<Fn>(fn)();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::forward<Fn>(fn)();
    accumMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Lightweight RAII scope guard — runs a callable on scope exit.
template<typename Fn> struct ScopeExit
{
    explicit ScopeExit(Fn&& fn) : m_Fn(std::move(fn))
    {}
    // noexcept: exceptions must not escape destructors. The callables used here
    // (trace logging accumulations) are non-throwing; the try/catch is a safety
    // net that prevents std::terminate if that assumption is ever violated.
    ~ScopeExit() noexcept
    {
        try
        {
            m_Fn();
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- intentional: keep destructor noexcept
        {}
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

  private:
    Fn m_Fn;
};

// Hit-test callback:
// - Windows: always NORMAL; drag/resize handled entirely client-side (avoids modal move/resize stalls).
// - X11/XWayland: NORMAL for title bar and controls so SDL delivers mouse button events to the
//   app and drag is handled client-side (matches Windows); RESIZE_* for window edges so the WM
//   handles native resize.
// - Native Wayland: same RESIZE_* border handling, but the title-bar drag area itself returns
//   DRAGGABLE (compositor-managed drag, since client-side SDL_SetWindowPosition() doesn't work
//   there -- #744). SDL consumes the button-down event entirely for any non-NORMAL hit-test
//   result, so this file's onSDLEvent()-driven click/drag/double-click handling never runs for
//   that area on native Wayland -- only the compositor sees the press. Buttons stay NORMAL on
//   every backend so DRAGGABLE never consumes their clicks.
SDL_HitTestResult hitTestCallback(SDL_Window* sdlWindow, const SDL_Point* area, void* data)
{
#ifdef _WIN32
    (void) sdlWindow;
    (void) area;
    (void) data;
    // Always return NORMAL so SDL does not enter native modal move/resize loops
    // that pause rendering. Drag/resize is handled in TitleBarLayer::onSDLEvent.
    return SDL_HITTEST_NORMAL;
#else
    auto* layer = static_cast<TitleBarLayer*>(data);
    if (layer == nullptr || area == nullptr)
    {
        return SDL_HITTEST_NORMAL;
    }

    const auto x = static_cast<float>(area->x);
    const auto y = static_cast<float>(area->y);
    const float titleBarHeight = TitleBarLayer::height();

    int windowWidth = 0;
    int windowHeight = 0;
    if (!SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight))
    {
        spdlog::error("SDL_GetWindowSize failed in hitTestCallback: {}", SDL_GetError());
        return SDL_HITTEST_NORMAL;
    }

    const bool isMaximized = ((SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_MAXIMIZED) != 0);

    // The actual decision tree lives in computeWindowHitTest() (TitleBarGeometry.h), pure and
    // header-only so its ordering -- buttons must be excluded before any resize-border check,
    // since a button can sit within RESIZE_BORDER_THICKNESS of a window edge -- is directly
    // unit-testable without live SDL_Window/TitleBarLayer state (see #750 review). On native
    // Wayland the empty drag area hands off to the compositor (#744): client-side
    // SDL_SetWindowPosition() during a drag doesn't work there, and since SDL consumes the
    // button-down event entirely for any non-NORMAL result, this file's onSDLEvent()-driven
    // click/drag/double-click handling never runs for that area there -- only the compositor
    // sees the press, which means double-click-to-maximize doesn't fire from the title bar on
    // native Wayland (the maximize button remains available). X11/XWayland/Windows stay NORMAL
    // so SDL_EVENT_MOUSE_BUTTON_DOWN with clicks==2 is delivered reliably for double-click
    // maximize/restore detection there.
    return computeWindowHitTest(x,
                                y,
                                windowWidth,
                                windowHeight,
                                titleBarHeight,
                                resizeBorderThickness(),
                                isMaximized,
                                layer->isPointInControlArea(x, y),
                                Core::VideoBackend::isWayland());
#endif
}

} // namespace

TitleBarLayer::TitleBarLayer() : Layer("TitleBarLayer")
{}

TitleBarLayer::~TitleBarLayer() = default;

void TitleBarLayer::onAttach()
{
    spdlog::info("TitleBarLayer attached");

    // Initialize button bounds based on initial window size in LOGICAL coordinates
    // ImGui positions are in logical coordinates (DPI handled by SDL backend)
    // These will be updated every frame in renderTitleBar(), but we need
    // reasonable initial values for the hit test callback on first frame
    SDL_Window* sdlWindow = Core::Application::get().getWindow().getHandle();
    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);

    // Must match renderTitleBar()'s drawing exactly, or the click targets drift away from the
    // painted buttons -- both derive from the same helper for that reason.
    const float titleBarHeight = height();
    m_ButtonLayout = computeTitleBarButtonLayout(static_cast<float>(windowWidth),
                                                 computeTitleBarButtonWidth(titleBarHeight, TITLE_BAR_BUTTON_ASPECT),
                                                 titleBarHeight,
                                                 titleBarHeight * TITLE_BAR_SEPARATOR_GAP_RATIO);

    // The icon texture is loaded by the first renderTitleBar(), at the size the bar draws it; ImGui
    // only knows the framebuffer scale from the first frame on (#1169).

    m_TraceEnabled = Core::isEnvFlagEnabled(SDL_getenv("TASKSMACK_TRACE_RESIZE_PERF"));

    // Set up hit test for window dragging
    setupHitTest();
    createSystemCursors();
}

void TitleBarLayer::loadIconTexture(const int pixelSize)
{
    // Remembered even when loading fails, so a missing file is not retried every frame.
    m_IconTexturePx = pixelSize;
    const auto iconPath = UI::findAssetsDir() / "icons" / std::format("tasksmack-{}.png", pixelSize);
    m_IconTexture = UI::loadTexture(iconPath);
    if (m_IconTexture.valid())
    {
        spdlog::info("Loaded title bar icon: {}x{}", static_cast<int>(m_IconTexture.size().x), static_cast<int>(m_IconTexture.size().y));
    }
    else
    {
        spdlog::warn("Failed to load title bar icon from {}", Core::pathToUtf8(iconPath));
    }
}

void TitleBarLayer::onDetach()
{
    spdlog::info("TitleBarLayer detached");

    // Release the OpenGL resource now, while the GL context is still guaranteed valid,
    // rather than deferring to the (default) destructor's timing.
    m_IconTexture = UI::Texture{};

    destroySystemCursors();

    // Remove hit test callback
    Core::Application::get().getWindow().setHitTestCallback(nullptr, nullptr);
}

void TitleBarLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    updateWindowInteraction();
}

void TitleBarLayer::onPostRender()
{
    updateResizeCursor();
}

void TitleBarLayer::onSDLEvent(SDL_Event* event)
{
    if (event == nullptr)
    {
        return;
    }

    // A display was added, removed or changed mode, or its work area moved: re-read the usable
    // bounds the minimum window size is capped to on the next frame the bar is drawn (#1207).
    if (invalidatesUsableBounds(event->type))
    {
        m_MinimumSizeDisplayId = 0;
    }

    // Handle Alt+Space to open system menu
    // On Linux, Alt+Space may be consumed by the window manager, so we check both:
    // 1. Space pressed while Alt is held (normal case)
    // 2. Alt pressed while Space is held (alternate case)
    if (event->type == SDL_EVENT_KEY_DOWN)
    {
        const auto& keyEvent = event->key;
        const bool altPressed = (keyEvent.mod & (SDL_KMOD_LALT | SDL_KMOD_RALT)) != 0;

        // Check keyboard state for the key that wasn't just pressed
        int numKeys = 0;
        const bool* keyboardState = SDL_GetKeyboardState(&numKeys);
        bool spaceHeld = false;
        if (keyboardState != nullptr && SDL_SCANCODE_SPACE >= 0 && SDL_SCANCODE_SPACE < numKeys)
        {
            spaceHeld = keyboardState[SDL_SCANCODE_SPACE];
        }

        const bool ctrlPressed = (keyEvent.mod & (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL)) != 0;

        // Case 1: Space pressed while Alt is held
        if (keyEvent.key == SDLK_SPACE && altPressed)
        {
            spdlog::info("Alt+Space detected (Space pressed with Alt), opening system menu");
            m_ShowSystemMenu = true;
        }
        // Case 2: Alt pressed while Space is held (workaround for Linux WM consuming Alt+Space)
        else if ((keyEvent.key == SDLK_LALT || keyEvent.key == SDLK_RALT) && spaceHeld)
        {
            spdlog::info("Alt+Space detected (Alt pressed with Space), opening system menu");
            m_ShowSystemMenu = true;
        }
        // F10 no longer opens this menu: it is the htop-style Quit key (ShellLayer, #170).
        // Case 3: Ctrl+Space - alternative shortcut
        else if (keyEvent.key == SDLK_SPACE && ctrlPressed && !altPressed)
        {
            spdlog::info("Ctrl+Space detected, opening system menu");
            m_ShowSystemMenu = true;
        }
    }

    // On Windows and X11/XWayland, the title bar drag area returns SDL_HITTEST_NORMAL
    // (see hitTestCallback), so SDL_EVENT_MOUSE_BUTTON_DOWN is delivered and handled here
    // via SDL's built-in clicks field for double-click detection and the existing
    // client-side drag/resize interaction for window movement. On native Wayland the drag
    // area returns SDL_HITTEST_DRAGGABLE instead, so SDL consumes the button-down there and
    // this handler never runs for that area -- drag and double-click-to-maximize are the
    // compositor's responsibility on that backend (see hitTestCallback and #744).
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT)
    {
        if (event->button.clicks == 2)
        {
            // Double-click on the title bar: cancel any drag started by the first
            // click and toggle maximize / restore (mirrors OS caption double-click).
            endWindowInteraction();
            handleTitleBarDoubleClick(*event);
        }
        else
        {
            beginWindowInteraction(*event);
        }
    }
    else if ((event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT) ||
             event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
        endWindowInteraction();
    }
}

auto TitleBarLayer::isPointInControlArea(float x, float y) const -> bool
{
    // Called from the mouse-move hot path (updateResizeCursor(), hitTestCallback()) - test
    // each bound directly rather than building an array of ButtonBounds copies per call.
    return computeIsPointInBounds(x, y, m_IconBounds) || computeIsPointOnTitleBarButton(x, y, m_ButtonLayout);
}

auto TitleBarLayer::detectResizeEdge(float x, float y, int windowWidth, int windowHeight, bool isMaximized) -> ResizeEdge
{
    return computeDetectResizeEdge(x, y, windowWidth, windowHeight, isMaximized, resizeBorderThickness());
}

void TitleBarLayer::handleTitleBarDoubleClick(const SDL_Event& event) const
{
    auto& window = Core::Application::get().getWindow();
    SDL_Window* sdlWindow = window.getHandle();
    if (sdlWindow == nullptr)
    {
        return;
    }

    const SDL_WindowID thisWindowId = SDL_GetWindowID(sdlWindow);
    if (event.button.windowID != thisWindowId)
    {
        return;
    }

    const float mouseX = event.button.x;
    const float mouseY = event.button.y;

    // Only act on double-clicks in the title bar, not on a control or resize edge.
    if (isPointInControlArea(mouseX, mouseY))
    {
        return;
    }
    if (mouseY > height())
    {
        return;
    }
    const auto [windowWidth, windowHeight] = window.getSize();
    const bool isMaximized = window.isMaximized();
    if (detectResizeEdge(mouseX, mouseY, windowWidth, windowHeight, isMaximized) != ResizeEdge::None)
    {
        return;
    }

    // Toggle maximize / restore.
    if (isMaximized)
    {
        window.restore();
    }
    else
    {
        window.maximize();
    }
}

void TitleBarLayer::beginWindowInteraction(const SDL_Event& event)
{
    auto& window = Core::Application::get().getWindow();
    SDL_Window* sdlWindow = window.getHandle();
    if (sdlWindow == nullptr)
    {
        return;
    }

    const SDL_WindowID thisWindowId = SDL_GetWindowID(sdlWindow);
    if (event.button.windowID != thisWindowId)
    {
        return;
    }

    const float mouseX = event.button.x;
    const float mouseY = event.button.y;
    const auto [windowWidth, windowHeight] = window.getSize();
    const bool isMaximized = window.isMaximized();

    if (isPointInControlArea(mouseX, mouseY))
    {
        return;
    }

    const ResizeEdge edge = detectResizeEdge(mouseX, mouseY, windowWidth, windowHeight, isMaximized);

    // Compute the global mouse position from the window origin plus the event's
    // window-local coordinates. This is more accurate than calling
    // SDL_GetGlobalMouseState() after the fact: on X11 events are buffered and
    // the pointer may have moved between the time SDL captured the button-down
    // and the time we process it, which causes a jump on the first drag frame.
    const auto [windowOriginX, windowOriginY] = window.getPosition();
    const int globalMouseX = windowOriginX + static_cast<int>(mouseX);
    const int globalMouseY = windowOriginY + static_cast<int>(mouseY);

    if (edge != ResizeEdge::None)
    {
        m_InteractionMode = InteractionMode::Resize;
        m_Resize.edge = edge;
        m_Resize.startMouseGlobalX = globalMouseX;
        m_Resize.startMouseGlobalY = globalMouseY;
        m_Resize.startWindowX = windowOriginX;
        m_Resize.startWindowY = windowOriginY;
        m_Resize.startWindowWidth = windowWidth;
        m_Resize.startWindowHeight = windowHeight;
        m_Resize.lastAppliedX = windowOriginX;
        m_Resize.lastAppliedY = windowOriginY;
        m_Resize.lastAppliedWidth = windowWidth;
        m_Resize.lastAppliedHeight = windowHeight;
        m_Resize.hasPendingCommit = false;
        m_Resize.pendingWidth = windowWidth;
        m_Resize.pendingHeight = windowHeight;
        m_Resize.lastSizeCommitTime = Core::Application::getTime();
        return;
    }

    if (mouseY <= height())
    {
        if (isMaximized)
        {
            // Don't restore yet — defer restore until the pointer has actually moved
            // past the drag threshold so a bare click doesn't unmaximize the window.
            m_Drag.pendingRestore = true;
            m_Drag.maximizedWindowX = windowOriginX;
            m_Drag.maximizedWindowWidth = windowWidth;
            m_InteractionMode = InteractionMode::Drag;
            m_Drag.startMouseGlobalX = globalMouseX;
            m_Drag.startMouseGlobalY = globalMouseY;
            // Placeholder positions replaced once restore actually happens.
            m_Drag.startWindowX = windowOriginX;
            m_Drag.startWindowY = windowOriginY;
            m_Drag.lastAppliedX = windowOriginX;
            m_Drag.lastAppliedY = windowOriginY;
            m_Resize.edge = ResizeEdge::None;
        }
        else
        {
            m_InteractionMode = InteractionMode::Drag;
            m_Drag.startMouseGlobalX = globalMouseX;
            m_Drag.startMouseGlobalY = globalMouseY;
            m_Drag.startWindowX = windowOriginX;
            m_Drag.startWindowY = windowOriginY;
            m_Drag.lastAppliedX = windowOriginX;
            m_Drag.lastAppliedY = windowOriginY;
            m_Resize.edge = ResizeEdge::None;
        }
    }
}

auto TitleBarLayer::queryMouseState() -> std::tuple<int, int, SDL_MouseButtonFlags>
{
    if (!Core::VideoBackend::supportsGlobalMouseState())
    {
        // Native Wayland: SDL_GetGlobalMouseState() is unreliable there, so use the window-local
        // query for both position and button mask -- the button mask stays accurate as long as
        // the pointer is over this window, which holds while a drag/resize we started is active.
        auto& window = Core::Application::get().getWindow();
        const auto [windowOriginX, windowOriginY] = window.getPosition();
        float localX = 0.0F;
        float localY = 0.0F;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&localX, &localY);
        const int globalMouseX = windowOriginX + static_cast<int>(localX);
        const int globalMouseY = windowOriginY + static_cast<int>(localY);
        return {globalMouseX, globalMouseY, buttons};
    }

    // X11, XWayland, and Windows: the local query's button mask goes stale once the pointer
    // leaves the window (no mouse capture here), which can leave a drag/resize started near an
    // edge stuck active after the button is released off-window. Use the global query for both
    // position and button mask, matching this project's pre-Wayland-support behavior.
    float globalMouseXF = 0.0F;
    float globalMouseYF = 0.0F;
    const SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(&globalMouseXF, &globalMouseYF);
    return {static_cast<int>(globalMouseXF), static_cast<int>(globalMouseYF), buttons};
}

// Called every frame while a custom drag or resize is in progress. Reads the
// current mouse state, computes the desired window position/size delta
// from the recorded start positions, applies rate-limited SDL calls, and
// fires a WindowResizedEvent when a size commit is issued. Ends the interaction
// (via endWindowInteraction) if the left mouse button is no longer held.
void TitleBarLayer::updateWindowInteraction()
{
    if (m_InteractionMode == InteractionMode::None)
    {
        return;
    }

    using Clock = std::chrono::steady_clock;
    constexpr double SLOW_TITLEBAR_UPDATE_MS = 250.0;
    const bool startedDrag = (m_InteractionMode == InteractionMode::Drag);
    const bool startedResize = (m_InteractionMode == InteractionMode::Resize);

    double mouseStateMs = 0.0;
    double restoreMs = 0.0;
    double setPositionMs = 0.0;
    double setSizeMs = 0.0;
    double raiseResizeEventMs = 0.0;

    const auto updateStart = m_TraceEnabled ? Clock::now() : Clock::time_point{};
    const ScopeExit traceScope{
        [&]
        {
            if (!m_TraceEnabled)
            {
                return;
            }
            const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - updateStart).count();
            if (totalMs < SLOW_TITLEBAR_UPDATE_MS)
            {
                return;
            }
            const char* mode = "none";
            if (startedDrag)
            {
                mode = "drag";
            }
            else if (startedResize)
            {
                mode = "resize";
            }
            spdlog::info("ResizePerfTitleBarSlowUpdate: mode={} edge={} total={:.3f} ms mouse={:.3f} ms restore={:.3f} ms setPos={:.3f} ms "
                         "setSize={:.3f} ms raiseResizeEvent={:.3f} ms",
                         mode,
                         static_cast<int>(m_Resize.edge),
                         totalMs,
                         mouseStateMs,
                         restoreMs,
                         setPositionMs,
                         setSizeMs,
                         raiseResizeEventMs);
        }};

    // Query the current pointer position and button mask using a single backend-appropriate
    // call (see queryMouseState()) rather than mixing a global position query with a local
    // button-state query, which desyncs on Wayland and goes stale off-window elsewhere.
    SDL_MouseButtonFlags mouseButtons = 0U;
    int globalMouseX = 0;
    int globalMouseY = 0;
    timedOp(m_TraceEnabled, mouseStateMs, [&] { std::tie(globalMouseX, globalMouseY, mouseButtons) = queryMouseState(); });

    if ((mouseButtons & SDL_BUTTON_LMASK) == 0U)
    {
        endWindowInteraction();
        return;
    }

    auto& window = Core::Application::get().getWindow();

    if (m_InteractionMode == InteractionMode::Drag)
    {
        updateDrag(globalMouseX, globalMouseY, window, restoreMs, setPositionMs);
    }
    else
    {
        updateResize(globalMouseX, globalMouseY, window, setPositionMs, setSizeMs, raiseResizeEventMs);
    }
}

void TitleBarLayer::updateDrag(const int mx, const int my, Core::Window& window, double& restoreMs, double& setPositionMs)
{
    const int dx = mx - m_Drag.startMouseGlobalX;
    const int dy = my - m_Drag.startMouseGlobalY;

    if (m_Drag.pendingRestore)
    {
        // Defer restore until mouse has moved at least DRAG_THRESHOLD pixels in
        // any direction so a bare click on the title bar does not unmaximize.
        constexpr int DRAG_THRESHOLD = 5;
        if (dx > -DRAG_THRESHOLD && dx < DRAG_THRESHOLD && dy > -DRAG_THRESHOLD && dy < DRAG_THRESHOLD)
        {
            return;
        }
        // Threshold crossed — restore and rebase the drag origin.
        timedOp(m_TraceEnabled, restoreMs, [&] { window.restore(); });
        const auto [restoredX, restoredY] = window.getPosition();
        const auto [restoredWidth, restoredHeight] = window.getSize();
        const int adjustedX =
            computeRestoreFromMaximizedDragX(m_Drag.startMouseGlobalX, m_Drag.maximizedWindowX, m_Drag.maximizedWindowWidth, restoredWidth);
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(adjustedX, restoredY); });
        Core::Application::get().signalWindowGeometryChanged();
        m_Drag.startWindowX = adjustedX;
        m_Drag.startWindowY = restoredY;
        m_Drag.lastAppliedX = adjustedX;
        m_Drag.lastAppliedY = restoredY;
        m_Drag.pendingRestore = false;
        (void) restoredX;
        (void) restoredHeight;
        return;
    }

    const int targetX = m_Drag.startWindowX + dx;
    const int targetY = m_Drag.startWindowY + dy;
    if (targetX != m_Drag.lastAppliedX || targetY != m_Drag.lastAppliedY)
    {
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(targetX, targetY); });
        Core::Application::get().signalWindowGeometryChanged();
        m_Drag.lastAppliedX = targetX;
        m_Drag.lastAppliedY = targetY;
    }
}

void TitleBarLayer::updateResize(
    const int mx, const int my, Core::Window& window, double& setPositionMs, double& setSizeMs, double& raiseResizeEventMs)
{
    const int dx = mx - m_Resize.startMouseGlobalX;
    const int dy = my - m_Resize.startMouseGlobalY;

    const auto [newX, newY, newWidth, newHeight] = computeResizeGeometry(m_Resize.edge,
                                                                         m_Resize.startWindowX,
                                                                         m_Resize.startWindowY,
                                                                         m_Resize.startWindowWidth,
                                                                         m_Resize.startWindowHeight,
                                                                         dx,
                                                                         dy,
                                                                         m_MinimumSize.width,
                                                                         m_MinimumSize.height);

    const bool positionChanged = (newX != m_Resize.lastAppliedX) || (newY != m_Resize.lastAppliedY);
    const bool sizeChanged = (newWidth != m_Resize.lastAppliedWidth) || (newHeight != m_Resize.lastAppliedHeight);
    const double now = Core::Application::getTime();
    bool sizeCommitApplied = false;

    if (positionChanged)
    {
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(newX, newY); });
        Core::Application::get().signalWindowGeometryChanged();
        m_Resize.lastAppliedX = newX;
        m_Resize.lastAppliedY = newY;
    }
    if (sizeChanged)
    {
        const bool commitIntervalElapsed = (now - m_Resize.lastSizeCommitTime) >= RESIZE_SIZE_COMMIT_INTERVAL_SECONDS;
        if (commitIntervalElapsed)
        {
            timedOp(m_TraceEnabled,
                    setSizeMs,
                    [&]
                    {
                        Core::traceResizePerfSDL(Core::ResizePerfOperation::SizeCommit,
                                                 newWidth,
                                                 newHeight,
                                                 [&] { return SDL_SetWindowSize(window.getHandle(), newWidth, newHeight); });
                    });
            Core::Application::get().signalWindowGeometryChanged();
            m_Resize.lastAppliedWidth = newWidth;
            m_Resize.lastAppliedHeight = newHeight;
            m_Resize.lastSizeCommitTime = now;
            m_Resize.hasPendingCommit = false;
            sizeCommitApplied = true;
        }
        else
        {
            m_Resize.hasPendingCommit = true;
            m_Resize.pendingWidth = newWidth;
            m_Resize.pendingHeight = newHeight;
        }
    }
    else if (m_Resize.hasPendingCommit)
    {
        // sizeChanged is false here: the current desired size already matches the last
        // committed size, meaning the user dragged back to the committed geometry.
        // The pending commit is now stale — applying it would jump the window to an
        // intermediate size the user no longer wants — so cancel it immediately.
        m_Resize.hasPendingCommit = false;
    }

    if (sizeCommitApplied)
    {
        // Coalesce immediate resize events to avoid flooding the event bus
        // during edge/corner drags. SDL will still emit pixel-size events,
        // so this path only provides low-latency updates for interactive drag.
        int pixelW = 0;
        int pixelH = 0;
        SDL_GetWindowSizeInPixels(window.getHandle(), &pixelW, &pixelH);
        constexpr double MIN_RESIZE_EVENT_INTERVAL_SECONDS = 1.0 / 120.0;
        const double resizeEventNow = Core::Application::getTime();
        const bool pixelSizeChanged = (pixelW != m_Resize.lastImmediatePixelW) || (pixelH != m_Resize.lastImmediatePixelH);
        const bool intervalElapsed = (resizeEventNow - m_Resize.lastImmediateEventTime) >= MIN_RESIZE_EVENT_INTERVAL_SECONDS;
        if (pixelSizeChanged && intervalElapsed)
        {
            m_Resize.lastImmediatePixelW = pixelW;
            m_Resize.lastImmediatePixelH = pixelH;
            m_Resize.lastImmediateEventTime = resizeEventNow;
            Core::WindowResizedEvent resizeEvent(pixelW, pixelH);
            timedOp(m_TraceEnabled, raiseResizeEventMs, [&] { Core::Application::get().raiseEvent(resizeEvent); });
        }
    }
}

void TitleBarLayer::endWindowInteraction()
{
    if (m_InteractionMode == InteractionMode::Resize && m_Resize.hasPendingCommit)
    {
        auto& window = Core::Application::get().getWindow();
        SDL_Window* sdlWindow = window.getHandle();
        if (sdlWindow != nullptr)
        {
            Core::traceResizePerfSDL(Core::ResizePerfOperation::FinalSizeCommit,
                                     m_Resize.pendingWidth,
                                     m_Resize.pendingHeight,
                                     [&] { return SDL_SetWindowSize(sdlWindow, m_Resize.pendingWidth, m_Resize.pendingHeight); });
        }
        // m_Resize will be zeroed below
    }

    m_Drag = {};
    m_Resize = {};
    m_InteractionMode = InteractionMode::None;
    m_HasCursorSample = false;
}

void TitleBarLayer::createSystemCursors()
{
    m_DefaultCursor = SDL_GetDefaultCursor();
    m_NsResizeCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    m_EwResizeCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    m_NeswResizeCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
    m_NwseResizeCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);

    // Diagnostic: SDL_CreateSystemCursor() returns nullptr on failure (e.g. a cursor theme
    // missing a directional resize shape), and applyCursorForEdge() silently falls back to the
    // default cursor whenever its target cursor is null -- indistinguishable at the UI level
    // from "no resize cursor at all". Surface creation failures explicitly (see #749 follow-up).
    if (m_NsResizeCursor == nullptr || m_EwResizeCursor == nullptr || m_NeswResizeCursor == nullptr || m_NwseResizeCursor == nullptr)
    {
        spdlog::warn("TitleBarLayer: one or more resize-cursor shapes failed to create (ns={} ew={} nesw={} nwse={}); "
                     "SDL error: {}",
                     m_NsResizeCursor != nullptr,
                     m_EwResizeCursor != nullptr,
                     m_NeswResizeCursor != nullptr,
                     m_NwseResizeCursor != nullptr,
                     SDL_GetError());
    }
}

void TitleBarLayer::destroySystemCursors()
{
    if (m_DefaultCursor != nullptr)
    {
        SDL_SetCursor(m_DefaultCursor);
    }

    if (m_NsResizeCursor != nullptr)
    {
        SDL_DestroyCursor(m_NsResizeCursor);
        m_NsResizeCursor = nullptr;
    }
    if (m_EwResizeCursor != nullptr)
    {
        SDL_DestroyCursor(m_EwResizeCursor);
        m_EwResizeCursor = nullptr;
    }
    if (m_NeswResizeCursor != nullptr)
    {
        SDL_DestroyCursor(m_NeswResizeCursor);
        m_NeswResizeCursor = nullptr;
    }
    if (m_NwseResizeCursor != nullptr)
    {
        SDL_DestroyCursor(m_NwseResizeCursor);
        m_NwseResizeCursor = nullptr;
    }
}

void TitleBarLayer::applyCursorForEdge(const ResizeEdge edge)
{
    SDL_Cursor* cursor = nullptr;

    switch (edge)
    {
    case ResizeEdge::Top:
    case ResizeEdge::Bottom:
        cursor = m_NsResizeCursor;
        break;
    case ResizeEdge::Left:
    case ResizeEdge::Right:
        cursor = m_EwResizeCursor;
        break;
    case ResizeEdge::TopLeft:
    case ResizeEdge::BottomRight:
        cursor = m_NwseResizeCursor;
        break;
    case ResizeEdge::TopRight:
    case ResizeEdge::BottomLeft:
        cursor = m_NeswResizeCursor;
        break;
    case ResizeEdge::None:
        cursor = m_DefaultCursor;
        break;
    }

    if (cursor == nullptr)
    {
        cursor = m_DefaultCursor;
    }
    if (cursor != nullptr)
    {
        SDL_SetCursor(cursor);
    }
}

void TitleBarLayer::updateResizeCursor()
{
    auto& window = Core::Application::get().getWindow();
    const SDL_Window* sdlWindow = window.getHandle();
    if (sdlWindow == nullptr)
    {
        return;
    }

    const ResizeEdge prevEdge = m_CachedHoverEdge;
    // Both active resize AND active drag must skip real hover sampling below: during a
    // client-side drag the window moves under the pointer, so the window-local mouse
    // coordinate can transiently cross into the resize-border zone and flip in a resize
    // cursor mid-drag if hover detection keeps running (see the #750-follow-up review).
    const bool isInteracting = (m_InteractionMode != InteractionMode::None);
    // SDL_GetMouseFocus() can transiently disagree with reality while the WM/compositor owns
    // an active hit-test-triggered resize or drag (border SDL_HITTEST_RESIZE_* / title-bar
    // SDL_HITTEST_DRAGGABLE on native Wayland) -- see #699, #749, and the #750 review. The
    // policy for what to do about it lives in computeResizeCursorUpdate() below.
    const bool focusMismatch = !isInteracting && (SDL_GetMouseFocus() != sdlWindow);

    bool hoverSampleAvailable = false;
    ResizeEdge hoverEdge = ResizeEdge::None;

    if (focusMismatch)
    {
        // Diagnostic: log only on transition into this branch. Added during the #749
        // follow-up investigation into a WSLg no-resize-cursor report, which turned out to
        // be a WSL session issue rather than this code path; kept to help diagnose any
        // future genuine Wayland-compositor cursor report.
        if (!m_HasLoggedFocusMismatch || !m_LastLoggedFocusMismatch)
        {
            spdlog::debug("updateResizeCursor: SDL_GetMouseFocus() != our window (holding cached edge={})", static_cast<int>(prevEdge));
            m_LastLoggedFocusMismatch = true;
            m_HasLoggedFocusMismatch = true;
        }
    }
    else if (!isInteracting)
    {
        if (!m_HasLoggedFocusMismatch || m_LastLoggedFocusMismatch)
        {
            spdlog::debug("updateResizeCursor: SDL_GetMouseFocus() matches our window (real hover detection active)");
            m_LastLoggedFocusMismatch = false;
            m_HasLoggedFocusMismatch = true;
        }

        // Use window-local coordinates from SDL_GetMouseState. Global
        // coordinates (SDL_GetGlobalMouseState) are unreliable on Wayland —
        // including WSLg — where compositors do not expose the global cursor
        // position, which left edge detection permanently stuck at None.
        float localX = 0.0F;
        float localY = 0.0F;
        SDL_GetMouseState(&localX, &localY);
        const int mouseLocalX = static_cast<int>(localX);
        const int mouseLocalY = static_cast<int>(localY);

        const auto [windowWidth, windowHeight] = window.getSize();
        const bool isMaximized = window.isMaximized();

        const bool stateUnchanged = m_HasCursorSample && mouseLocalX == m_LastCursorMouseLocalX && mouseLocalY == m_LastCursorMouseLocalY &&
                                    windowWidth == m_LastCursorWindowWidth && windowHeight == m_LastCursorWindowHeight &&
                                    isMaximized == m_LastCursorWindowMaximized;

        if (!stateUnchanged)
        {
            m_LastCursorMouseLocalX = mouseLocalX;
            m_LastCursorMouseLocalY = mouseLocalY;
            m_LastCursorWindowWidth = windowWidth;
            m_LastCursorWindowHeight = windowHeight;
            m_LastCursorWindowMaximized = isMaximized;
            m_HasCursorSample = true;
            hoverSampleAvailable = true;

            const bool insideWindow =
                (localX >= 0.0F && localY >= 0.0F && localX < static_cast<float>(windowWidth) && localY < static_cast<float>(windowHeight));
            if (insideWindow)
            {
                hoverEdge = detectResizeEdge(localX, localY, windowWidth, windowHeight, isMaximized);
                // Suppress the resize cursor over title-bar controls: on Windows all
                // resize is client-side, but controls should still show the default cursor.
                if (hoverEdge != ResizeEdge::None && isPointInControlArea(localX, localY))
                {
                    hoverEdge = ResizeEdge::None;
                }
            }
            // Diagnostic: log only edge transitions, not every changed hover sample --
            // moving the pointer changes coordinates far more often than it crosses an
            // edge boundary, and logging every sample risks distorting the very
            // resize/drag behavior being diagnosed. Added during the #749 follow-up
            // investigation (see comment above); kept for future Wayland diagnostics.
            if (hoverEdge != prevEdge)
            {
                spdlog::debug("updateResizeCursor: hover sample ({}, {}) in {}x{} window -> edge={}",
                              mouseLocalX,
                              mouseLocalY,
                              windowWidth,
                              windowHeight,
                              static_cast<int>(hoverEdge));
            }
        }
    }

    const auto update = computeResizeCursorUpdate(isInteracting, m_Resize.edge, focusMismatch, hoverSampleAvailable, hoverEdge, prevEdge);
    if (update.updateCachedEdge)
    {
        m_CachedHoverEdge = update.resolvedEdge;
    }
    if (update.applyCursor)
    {
        applyCursorForEdge(update.resolvedEdge);
    }
}

void TitleBarLayer::onRender()
{
    renderTitleBar();
}

void TitleBarLayer::setupHitTest()
{
    Core::Application::get().getWindow().setHitTestCallback(hitTestCallback, this);
}

void TitleBarLayer::renderTitleBar()
{
    auto& window = Core::Application::get().getWindow();
    const auto [windowWidth, windowHeight] = window.getSize();
    const float titleBarHeight = height();

    // The bundled icon nearest above the drawn size in framebuffer pixels; after a display-scale
    // change that is another file (#1169). Loading between NewFrame() and Render() is fine: the
    // texture is only sampled when the frame is drawn.
    if (const int wantedPx =
            selectIconPixelSize(TitleBarView::iconSize(titleBarHeight) * ImGui::GetIO().DisplayFramebufferScale.y, APP_ICON_PIXEL_SIZES);
        wantedPx != m_IconTexturePx)
    {
        loadIconTexture(wantedPx);
    }

    const TitleBarView::Output drawn = TitleBarView::render({.windowWidth = static_cast<float>(windowWidth),
                                                             .windowHeight = static_cast<float>(windowHeight),
                                                             .barHeight = titleBarHeight,
                                                             .maximized = window.isMaximized(),
                                                             .icon = &m_IconTexture,
                                                             .openSystemMenu = std::exchange(m_ShowSystemMenu, false)});
    // Where every button and the icon were drawn, and so what isPointInControlArea() keeps out of the
    // drag area and where a right-click opens the system menu.
    m_ButtonLayout = drawn.layout;
    if (drawn.iconDrawn)
    {
        m_IconBounds = drawn.iconBounds;
    }
    updateMinimumWindowSize(drawn.contentWidth);

    switch (drawn.action)
    {
    case TitleBarView::Action::IconClicked:
#ifdef _WIN32
        showNativeSystemMenu();
#endif
        break; // Linux: the view opened its own menu
    case TitleBarView::Action::Close:
        window.requestClose();
        break;
    case TitleBarView::Action::Maximize:
        window.maximize();
        break;
    case TitleBarView::Action::Restore:
        window.restore();
        break;
    case TitleBarView::Action::Minimize:
        window.minimize();
        break;
    case TitleBarView::Action::Settings:
    {
        Core::OpenSettingsEvent event;
        Core::Application::get().raiseEvent(event);
        break;
    }
    case TitleBarView::Action::Help:
    {
        Core::OpenHelpEvent event;
        Core::Application::get().raiseEvent(event);
        break;
    }
    case TitleBarView::Action::About:
    {
        Core::OpenAboutEvent event;
        Core::Application::get().raiseEvent(event);
        break;
    }
    case TitleBarView::Action::None:
        break;
    }
}

void TitleBarLayer::updateMinimumWindowSize(float contentWidth)
{
    // The window may not be made narrower than what this bar or the panels below it have to show
    // (#1207), or shorter than the panels' content (#1278) or the base minimum at this display scale. Derived from the sizes just used for
    // drawing, so it cannot drift from them, and handed to SDL only when it changes (#970). ShellLayer has already set the scaled base
    // minimum at attach; this widens it to cover the bar.
    auto& window = Core::Application::get().getWindow();
    const WindowMinimumSize desiredMinimumSize =
        computeMinimumWindowSize(UI::Theme::get().displayScale(), contentWidth, m_ContentMinimumWidthPx, m_ContentMinimumHeightPx);
    // Held inside the current display's usable bounds, or a large font on a small display would
    // leave a window that cannot fit on-screen or be maximized (#1207). The bounds are read only
    // when the wanted minimum or the display changes.
    const SDL_DisplayID displayId = window.getDisplayId();
    if (desiredMinimumSize != m_DesiredMinimumSize || displayId != m_MinimumSizeDisplayId)
    {
        m_DesiredMinimumSize = desiredMinimumSize;
        m_MinimumSizeDisplayId = displayId;
        const auto [usableWidth, usableHeight] = window.getUsableDisplaySize().value_or(std::pair{0, 0});
        const WindowMinimumSize minimumSize = capMinimumToUsable(desiredMinimumSize, usableWidth, usableHeight);
        if (minimumSize != m_MinimumSize)
        {
            m_MinimumSize = minimumSize;
            if (!SDL_SetWindowMinimumSize(window.getHandle(), minimumSize.width, minimumSize.height))
            {
                spdlog::warn("SDL_SetWindowMinimumSize({}, {}) failed: {}", minimumSize.width, minimumSize.height, SDL_GetError());
            }
        }
    }
}

#ifdef _WIN32
void TitleBarLayer::showNativeSystemMenu()
{
    // Show native Windows system menu
    auto* sdlWindow = Core::Application::get().getWindow().getHandle();
    SDL_PropertiesID const props = SDL_GetWindowProperties(sdlWindow);
    auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

    if (hwnd != nullptr)
    {
        HMENU systemMenu = GetSystemMenu(hwnd, FALSE);
        if (systemMenu != nullptr)
        {
            // Get cursor position for menu display
            POINT pt;
            GetCursorPos(&pt);

            // Track the menu command
            int const cmd = TrackPopupMenu(systemMenu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            if (cmd != 0)
            {
                PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
            }
        }
    }
}
#endif

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) - Intentionally non-static for OOP consistency

} // namespace App
