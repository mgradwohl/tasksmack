#include "TitleBarLayer.h"

#include "App/TitleBarGeometry.h"
#include "App/TitleBarView.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/EnvRead.h"
#include "Core/Layer.h"
#include "Core/ResizePerfOperation.h"
#include "Core/Utf8Path.h"
#include "Core/VideoBackend.h"
#include "Core/WindowEvents.h"
#include "Core/WindowGeometry.h"
#include "UI/AssetPath.h"
#include "UI/IconLoader.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <optional>
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

// Hit-test callback, installed on the window by setupHitTest(). The decision is
// TitleBarLayer::hitTest() -- computeWindowHitTestAt() over the geometry the bar drew with and the
// backend's policy (Core::VideoBackend::usesSystemDrag()/usesSystemResize()) -- so what the OS asks
// here and what a test asks are one function. Per backend:
// - Windows (#2716): the empty title bar answers DRAGGABLE, which SDL turns into HTCAPTION, so
//   Windows runs the move itself: Aero Snap to a screen edge, the snap preview, drag-to-the-top to
//   maximize and a double-click to maximize all come from that loop. The borders stay NORMAL and the
//   resize is client-side (updateResize()): the OS resize loop is modal inside SDL's event pump, and
//   nothing would be drawn until the button came up. A client-side maximized window (#1208) is a
//   normal window to the OS, so its title bar stays NORMAL too and the drag that restores it under
//   the pointer stays client-side (#2715).
// - X11/XWayland: NORMAL for the title bar, so SDL delivers the press and the drag is client-side;
//   RESIZE_* for the borders, so the window manager resizes.
// - Native Wayland: RESIZE_* for the borders and DRAGGABLE for the empty title bar (compositor
//   drag; client-side SDL_SetWindowPosition() doesn't work there -- #744).
// SDL consumes the button-down event entirely for any non-NORMAL answer, so this file's
// onSDLEvent()-driven click/drag/double-click handling never runs for that area: only the OS or
// compositor sees the press. Buttons stay NORMAL on every backend so DRAGGABLE never consumes
// their clicks (#750).
SDL_HitTestResult hitTestCallback(SDL_Window* /*sdlWindow*/, const SDL_Point* area, void* data)
{
    const auto* layer = static_cast<const TitleBarLayer*>(data);
    if (layer == nullptr || area == nullptr)
    {
        return SDL_HITTEST_NORMAL;
    }
    return layer->hitTest(static_cast<float>(area->x), static_cast<float>(area->y));
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

    m_TraceEnabled = Core::resizePerfTraceRequested();

    // Hit testing: who runs a title-bar drag and a border resize (see hitTestCallback).
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

    // Where the hit test answers SDL_HITTEST_NORMAL (see hitTestCallback: the title bar on
    // X11/XWayland and on a client-side maximized window on Windows, the borders on Windows),
    // SDL_EVENT_MOUSE_BUTTON_DOWN is delivered and handled here via SDL's built-in clicks field
    // for double-click detection and the client-side drag/resize interaction for window movement.
    // Where it answers DRAGGABLE or RESIZE_* (the title bar on native Wayland, #744, and on a
    // normal window on Windows, #2716; the borders off Windows), SDL consumes the button-down and
    // this handler never runs for that area: the drag, the resize and a double-click to maximize
    // are the OS's or the compositor's there.
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
    // The same decision the hit-test callback makes over the same geometry: the buttons and the
    // icon, less the top-right resize corner the Close button's slot reaches into (#2882).
    return computeIsPointInControlArea(x, y, hitTestInput());
}

auto TitleBarLayer::detectResizeEdge(float x, float y, int windowWidth, int windowHeight, bool isMaximized) -> ResizeEdge
{
    return computeDetectResizeEdge(x, y, windowWidth, windowHeight, isMaximized, resizeBorderThickness());
}

auto TitleBarLayer::hitTestInput() const -> WindowHitTestInput
{
    const Core::Window& window = Core::Application::get().getWindow();
    const auto [windowWidth, windowHeight] = window.getSize();
    // The window's own answer, not SDL_WINDOW_MAXIMIZED: a client-side maximize (Windows, #1208; X11
    // without EWMH maximize, #1339) never sets that flag, and its edges would stay resize handles
    // nobody serves (#2497). SDL calls the hit test from its event pump, on the main thread.
    const bool isMaximized = window.isMaximized();
    return {.windowWidth = windowWidth,
            .windowHeight = windowHeight,
            .titleBarHeightPx = height(),
            .resizeBorderPx = resizeBorderThickness(),
            .buttons = m_ButtonLayout,
            .icon = m_IconBounds,
            .isMaximized = isMaximized,
            .systemDrag = computeSystemDragApplies(
                Core::VideoBackend::usesSystemDrag(), Core::VideoBackend::supportsClientSideMaximize(), isMaximized),
            .systemResize = Core::VideoBackend::usesSystemResize()};
}

auto TitleBarLayer::hitTest(const float x, const float y) const -> SDL_HitTestResult
{
    return computeWindowHitTestAt(x, y, hitTestInput());
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
        m_RestoreSettle.stop();
        m_Resize.begin(edge,
                       globalMouseX,
                       globalMouseY,
                       {.x = windowOriginX, .y = windowOriginY, .width = windowWidth, .height = windowHeight},
                       Core::Application::getTime());
        return;
    }

    if (mouseY <= height())
    {
        // On a maximized window the restore waits until the pointer has actually moved past the drag
        // threshold, so a bare click doesn't unmaximize the window.
        m_InteractionMode = InteractionMode::Drag;
        m_RestoreSettle.stop();
        m_Drag.begin(globalMouseX, globalMouseY, windowOriginX, windowOriginY, isMaximized, windowWidth);
        m_Resize = {};
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
        if (m_RestoreSettle.active())
        {
            // A quick drag off a maximize can end before the window manager's late restore lands.
            double setPositionMs = 0.0;
            settleRestore(Core::Application::get().getWindow(), setPositionMs);
        }
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
                         static_cast<int>(m_Resize.edge()),
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
    timedOp(m_TraceEnabled,
            mouseStateMs,
            [&]
            {
                std::tie(globalMouseX, globalMouseY, mouseButtons) =
                    m_MouseStateQueryForTesting ? m_MouseStateQueryForTesting() : queryMouseState();
            });

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
    const DragTracker::Step step = m_Drag.step(mx, my);
    switch (step.kind)
    {
    case DragTracker::Step::Kind::None:
        // A moving pointer moves the window itself; a still one may need it moved back.
        settleRestore(window, setPositionMs);
        break;
    case DragTracker::Step::Kind::Restore:
    {
        // Threshold crossed: restore, and carry on the drag from where the restored window lands.
        // Where it would have restored to, which an X11 window manager may still move it to (#2761).
        const std::optional<Core::WindowGeometry::Rect> staleRestore = window.getNormalGeometry();
        timedOp(m_TraceEnabled, restoreMs, [&] { window.restore(); });
        // Only the restored width is used: the saved restore position is replaced by one that keeps the
        // pointer at the spot on the title bar it pressed, across and down (#2715).
        const int restoredWidth = window.getSize().first;
        const auto [adjustedX, adjustedY] = m_Drag.rebaseAfterRestore(restoredWidth, mx, my);
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(adjustedX, adjustedY); });
        // X11 only: elsewhere the restore is synchronous, or (native Wayland) the compositor drags.
        if (staleRestore.has_value() && (Core::VideoBackend::isX11() || Core::VideoBackend::isXWaylandFallback()))
        {
            m_RestoreSettle.start(adjustedX, adjustedY, staleRestore->x, staleRestore->y, Core::Application::getTime());
        }
        Core::Application::get().signalWindowGeometryChanged();
        break;
    }
    case DragTracker::Step::Kind::Move:
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(step.x, step.y); });
        m_RestoreSettle.retarget(step.x, step.y);
        Core::Application::get().signalWindowGeometryChanged();
        break;
    }
}

void TitleBarLayer::settleRestore(Core::Window& window, double& setPositionMs)
{
    if (!m_RestoreSettle.active())
    {
        return;
    }
    if (!window.isNormalNow())
    {
        m_RestoreSettle.stop();
        return;
    }
    const auto [liveX, liveY] = window.getPosition();
    if (const auto back = m_RestoreSettle.check(liveX, liveY, Core::Application::getTime()); back.has_value())
    {
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(back->first, back->second); });
        Core::Application::get().signalWindowGeometryChanged();
    }
}

void TitleBarLayer::updateResize(
    const int mx, const int my, Core::Window& window, double& setPositionMs, double& setSizeMs, double& raiseResizeEventMs)
{
    // The minimum SDL holds the window to, not the wanted one: on a side the window is still smaller
    // than the wanted minimum, the drag keeps the window's size there rather than jumping it (#1945).
    const auto [minimumWidth, minimumHeight] = window.getMinimumSize();
    const ResizeTracker::Step step = m_Resize.step(mx, my, minimumWidth, minimumHeight, Core::Application::getTime());
    if (step.move)
    {
        timedOp(m_TraceEnabled, setPositionMs, [&] { window.setPosition(step.x, step.y); });
        Core::Application::get().signalWindowGeometryChanged();
    }
    if (!step.commitSize)
    {
        return;
    }
    timedOp(m_TraceEnabled,
            setSizeMs,
            [&]
            {
                Core::traceResizePerfSDL(Core::ResizePerfOperation::SizeCommit,
                                         step.width,
                                         step.height,
                                         [&] { return SDL_SetWindowSize(window.getHandle(), step.width, step.height); });
            });
    Core::Application::get().signalWindowGeometryChanged();

    // Coalesce immediate resize events to avoid flooding the event bus during edge/corner drags. SDL
    // will still emit pixel-size events, so this path only provides low-latency updates for
    // interactive drag.
    int pixelW = 0;
    int pixelH = 0;
    SDL_GetWindowSizeInPixels(window.getHandle(), &pixelW, &pixelH);
    if (m_Resize.takeResizeEvent(pixelW, pixelH, Core::Application::getTime()))
    {
        Core::WindowResizedEvent resizeEvent(pixelW, pixelH);
        timedOp(m_TraceEnabled, raiseResizeEventMs, [&] { Core::Application::get().raiseEvent(resizeEvent); });
    }
}

void TitleBarLayer::endWindowInteraction()
{
    if (const std::optional<std::pair<int, int>> pending = m_Resize.pendingSize();
        m_InteractionMode == InteractionMode::Resize && pending.has_value())
    {
        if (SDL_Window* sdlWindow = Core::Application::get().getWindow().getHandle(); sdlWindow != nullptr)
        {
            const auto [width, height] = *pending;
            Core::traceResizePerfSDL(
                Core::ResizePerfOperation::FinalSizeCommit, width, height, [&] { return SDL_SetWindowSize(sdlWindow, width, height); });
        }
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

    const auto update = computeResizeCursorUpdate(isInteracting, m_Resize.edge(), focusMismatch, hoverSampleAvailable, hoverEdge, prevEdge);
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
                                                             .resizeBorderPx = resizeBorderThickness(),
                                                             .icon = &m_IconTexture,
                                                             .openSystemMenu = std::exchange(m_ShowSystemMenu, false),
                                                             .title = window.title()});
    // Where every button and the icon were drawn, and so what isPointInControlArea() keeps out of the
    // drag area and where a right-click opens the system menu.
    m_ButtonLayout = drawn.layout;
    if (drawn.iconDrawn)
    {
        m_IconBounds = drawn.iconBounds;
    }
    updateMinimumWindowSize(drawn.contentWidth);

#ifdef _WIN32
    // The icon opens the native system menu, and what it chose is carried out below, as the buttons'
    // actions are (#3161).
    const TitleBarView::Action action =
        drawn.action == TitleBarView::Action::IconClicked
            ? showNativeSystemMenu(window.isMaximized()) // LCOV_EXCL_LINE - the modal native menu, which a test can't dismiss (#2039)
            : drawn.action;
#else
    const TitleBarView::Action action = drawn.action;
#endif
    switch (action)
    {
    case TitleBarView::Action::IconClicked:
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
    const float displayScale = UI::Theme::get().displayScale();
    const WindowMinimumSize desiredMinimumSize =
        computeMinimumWindowSize(displayScale, contentWidth, m_ContentMinimumWidthPx, m_ContentMinimumHeightPx);
    // Without what the tabs measured: the part that moves only with the font, the scale or the
    // display, and may grow the window (#1945).
    const WindowMinimumSize desiredGrowMinimumSize =
        computeMinimumWindowSize(displayScale, contentWidth, m_ContentMinimumWidthPx, m_ContentEstimatedHeightPx);
    // Held inside the current display's usable bounds, or a large font on a small display would
    // leave a window that cannot fit on-screen or be maximized (#1207). The bounds are read only
    // when the wanted minimum or the display changes.
    const SDL_DisplayID displayId = window.getDisplayId();
    if (desiredMinimumSize != m_DesiredMinimumSize || desiredGrowMinimumSize != m_DesiredGrowMinimumSize ||
        displayId != m_MinimumSizeDisplayId)
    {
        m_DesiredMinimumSize = desiredMinimumSize;
        m_DesiredGrowMinimumSize = desiredGrowMinimumSize;
        m_MinimumSizeDisplayId = displayId;
        const auto [usableWidth, usableHeight] = window.getUsableDisplaySize().value_or(std::pair{0, 0});
        const WindowMinimumSize minimumSize = capMinimumToUsable(desiredMinimumSize, usableWidth, usableHeight);
        const WindowMinimumSize growMinimumSize = capMinimumToUsable(desiredGrowMinimumSize, usableWidth, usableHeight);
        // Grows the window at startup and when the font, the scale or the display moved the minimum;
        // keeps its size when a tab's measurement alone did (#1945).
        const Core::WindowGeometry::MinimumSizeChange change = minimumSizeChangeFor(m_GrowMinimumSize, growMinimumSize);
        if (minimumSize != m_MinimumSize || change == Core::WindowGeometry::MinimumSizeChange::AllowGrow)
        {
            m_MinimumSize = minimumSize;
            m_GrowMinimumSize = growMinimumSize;
            // Logs its own failure; keeps the current size on a tiled Wayland window (#1777).
            static_cast<void>(window.setMinimumSize(minimumSize.width, minimumSize.height, change));
        }
    }
}

#ifdef _WIN32
// LCOV_EXCL_START - TrackPopupMenu runs a modal menu loop until the user picks or dismisses (#2039)
TitleBarView::Action TitleBarLayer::showNativeSystemMenu(bool maximized)
{
    auto* sdlWindow = Core::Application::get().getWindow().getHandle();
    SDL_PropertiesID const props = SDL_GetWindowProperties(sdlWindow);
    auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    HMENU systemMenu = hwnd != nullptr ? GetSystemMenu(hwnd, FALSE) : nullptr;
    if (systemMenu == nullptr)
    {
        return TitleBarView::Action::None;
    }

    // Windows enables the items from the OS's own maximized state, which a client-side maximize
    // doesn't set: it offered Maximize, not Restore, on a maximized window (#3161). Set them as the
    // Linux menu has them.
    const TitleBarView::SystemMenuItems items = TitleBarView::systemMenuItems(maximized);
    const auto enable = [systemMenu](UINT command, bool enabled)
    {
        // Returns the item's previous state, or -1 for a menu without it; either way there is nothing to do.
        static_cast<void>(EnableMenuItem(systemMenu, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED)));
    };
    enable(SC_RESTORE, items.restore);
    enable(SC_MOVE, items.move);
    enable(SC_SIZE, items.size);
    enable(SC_MINIMIZE, items.minimize);
    enable(SC_MAXIMIZE, items.maximize);
    enable(SC_CLOSE, items.close);

    // At the cursor, which is on the icon that was clicked.
    POINT pt{};
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(systemMenu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    if (cmd == 0)
    {
        return TitleBarView::Action::None; // dismissed
    }
    const TitleBarView::Action action =
        TitleBarView::actionForSystemCommand(Core::WindowGeometry::decodeSystemCommand(static_cast<UINT_PTR>(cmd)));
    if (action == TitleBarView::Action::None)
    {
        // Move and Size: Windows' own keyboard move and size loops.
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
    }
    return action;
}
// LCOV_EXCL_STOP
#endif

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) - Intentionally non-static for OOP consistency

} // namespace App
