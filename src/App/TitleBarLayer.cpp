#include "TitleBarLayer.h"

#include "App/TitleBarGeometry.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Layer.h"
#include "Core/WindowEvents.h"
#include "UI/AssetPath.h"
#include "UI/IconLoader.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <ratio>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace App
{

// Height matches the tab bar row height in ShellLayer
// Tab bar uses FramePadding(16, 10) + TOP_EDGE_PADDING(4) = GetFrameHeight() + 24px
auto TitleBarLayer::height() -> float
{
    // Match ShellLayer tab bar: base frame height + vertical padding (10*2) + top edge padding (4)
    constexpr float TAB_BAR_VERTICAL_PADDING = 10.0F * 2.0F; // FramePadding.y * 2
    constexpr float TAB_BAR_TOP_PADDING = 4.0F;              // TOP_EDGE_PADDING
    return ImGui::GetFrameHeight() + TAB_BAR_VERTICAL_PADDING + TAB_BAR_TOP_PADDING;
}

namespace
{
[[nodiscard]] bool isResizePerfTracingEnabled() noexcept
{
    const char* value = SDL_getenv("TASKSMACK_TRACE_RESIZE_PERF");
    if (value == nullptr)
    {
        return false;
    }

    const std::string_view text(value);
    if (text.empty())
    {
        return false;
    }

    const auto ciEqual = [](const std::string_view a, const std::string_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }

        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))) != b[i])
            {
                return false;
            }
        }
        return true;
    };

    return !ciEqual(text, "0") && !ciEqual(text, "false") && !ciEqual(text, "off") && !ciEqual(text, "no");
}

// Helper to check if point is inside bounds
bool isInsideBounds(float x, float y, const TitleBarLayer::ButtonBounds& bounds)
{
    // Bounds must be valid (non-zero width)
    if (bounds.maxX <= bounds.minX)
    {
        return false;
    }
    return x >= bounds.minX && x <= bounds.maxX && y >= bounds.minY && y <= bounds.maxY;
}

// Shared resize border thickness — must stay in sync between hit-test and cursor detection.
constexpr float RESIZE_BORDER_THICKNESS = 8.0F;
constexpr float RESIZE_SIZE_COMMIT_INTERVAL_SECONDS = 1.0F / 20.0F;

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
// - Non-Windows: NORMAL for title bar and controls so SDL delivers mouse button events to the app;
//   RESIZE_* for window edges so the WM handles native resize. Drag is handled client-side too.
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
    if (!isMaximized)
    {
        if (y >= static_cast<float>(windowHeight) - RESIZE_BORDER_THICKNESS)
        {
            if (x < RESIZE_BORDER_THICKNESS)
            {
                return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            }
            if (x >= static_cast<float>(windowWidth) - RESIZE_BORDER_THICKNESS)
            {
                return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            }
            return SDL_HITTEST_RESIZE_BOTTOM;
        }

        if (x < RESIZE_BORDER_THICKNESS)
        {
            if (y < RESIZE_BORDER_THICKNESS)
            {
                return SDL_HITTEST_RESIZE_TOPLEFT;
            }
            return SDL_HITTEST_RESIZE_LEFT;
        }
        if (x >= static_cast<float>(windowWidth) - RESIZE_BORDER_THICKNESS)
        {
            if (y < RESIZE_BORDER_THICKNESS)
            {
                return SDL_HITTEST_RESIZE_TOPRIGHT;
            }
            return SDL_HITTEST_RESIZE_RIGHT;
        }

        if (y < RESIZE_BORDER_THICKNESS)
        {
            const auto& helpBounds = layer->getHelpBounds();
            if (helpBounds.maxX > helpBounds.minX && x >= helpBounds.minX)
            {
                return SDL_HITTEST_NORMAL;
            }
            return SDL_HITTEST_RESIZE_TOP;
        }
    }

    if (y > titleBarHeight)
    {
        return SDL_HITTEST_NORMAL;
    }

    // Title bar drag area — return NORMAL so SDL delivers mouse button events
    // to the app. Drag is handled client-side in beginWindowInteraction /
    // updateWindowInteraction, mirroring the Windows path. This ensures
    // SDL_EVENT_MOUSE_BUTTON_DOWN with clicks==2 is delivered reliably for
    // double-click maximize/restore detection.
    return SDL_HITTEST_NORMAL;
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

    constexpr float buttonWidth = 46.0F;
    const auto rightX = static_cast<float>(windowWidth);
    const float titleBarHeight = height();

    // Right to left: Close, Maximize, Minimize, (gap), Settings, Help
    float buttonX = rightX - buttonWidth;
    m_CloseBounds = {.minX = buttonX, .maxX = buttonX + buttonWidth, .minY = 0, .maxY = titleBarHeight};

    buttonX -= buttonWidth;
    m_MaximizeBounds = {.minX = buttonX, .maxX = buttonX + buttonWidth, .minY = 0, .maxY = titleBarHeight};

    buttonX -= buttonWidth;
    m_MinimizeBounds = {.minX = buttonX, .maxX = buttonX + buttonWidth, .minY = 0, .maxY = titleBarHeight};

    buttonX -= 16.0F; // Separator gap
    buttonX -= buttonWidth;
    m_SettingsBounds = {.minX = buttonX, .maxX = buttonX + buttonWidth, .minY = 0, .maxY = titleBarHeight};

    buttonX -= buttonWidth;
    m_HelpBounds = {.minX = buttonX, .maxX = buttonX + buttonWidth, .minY = 0, .maxY = titleBarHeight};

    // Load icon texture
    const auto iconPath = UI::findAssetsDir() / "icons" / "tasksmack-32.png";
    m_IconTexture = UI::loadTexture(iconPath);
    if (m_IconTexture.valid())
    {
        spdlog::info("Loaded title bar icon: {}x{}", static_cast<int>(m_IconTexture.size().x), static_cast<int>(m_IconTexture.size().y));
    }
    else
    {
        spdlog::warn("Failed to load title bar icon from {}", iconPath.string());
    }

    m_TraceEnabled = isResizePerfTracingEnabled();

    // Set up hit test for window dragging
    setupHitTest();
    createSystemCursors();
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
        // Case 3: F10 - standard menu activation key (works on all platforms)
        else if (keyEvent.key == SDLK_F10 && !altPressed && !ctrlPressed)
        {
            spdlog::info("F10 detected, opening system menu");
            m_ShowSystemMenu = true;
        }
        // Case 4: Ctrl+Space - alternative shortcut
        else if (keyEvent.key == SDLK_SPACE && ctrlPressed && !altPressed)
        {
            spdlog::info("Ctrl+Space detected, opening system menu");
            m_ShowSystemMenu = true;
        }
    }

    // Title bar drag area returns SDL_HITTEST_NORMAL (see hitTestCallback), so
    // SDL_EVENT_MOUSE_BUTTON_DOWN is delivered on all platforms. Use SDL's built-in
    // clicks field for double-click detection and the existing client-side drag/resize
    // interaction for window movement.
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
    return isInsideBounds(x, y, m_IconBounds) || isInsideBounds(x, y, m_HelpBounds) || isInsideBounds(x, y, m_SettingsBounds) ||
           isInsideBounds(x, y, m_MinimizeBounds) || isInsideBounds(x, y, m_MaximizeBounds) || isInsideBounds(x, y, m_CloseBounds);
}

auto TitleBarLayer::detectResizeEdge(float x, float y, int windowWidth, int windowHeight, bool isMaximized) -> ResizeEdge
{
    if (isMaximized)
    {
        return ResizeEdge::None;
    }

    const bool nearLeft = x < RESIZE_BORDER_THICKNESS;
    const bool nearRight = x >= (static_cast<float>(windowWidth) - RESIZE_BORDER_THICKNESS);
    const bool nearTop = y < RESIZE_BORDER_THICKNESS;
    const bool nearBottom = y >= (static_cast<float>(windowHeight) - RESIZE_BORDER_THICKNESS);

    if (nearTop && nearLeft)
    {
        return ResizeEdge::TopLeft;
    }
    if (nearTop && nearRight)
    {
        return ResizeEdge::TopRight;
    }
    if (nearBottom && nearLeft)
    {
        return ResizeEdge::BottomLeft;
    }
    if (nearBottom && nearRight)
    {
        return ResizeEdge::BottomRight;
    }
    if (nearLeft)
    {
        return ResizeEdge::Left;
    }
    if (nearRight)
    {
        return ResizeEdge::Right;
    }
    if (nearTop)
    {
        return ResizeEdge::Top;
    }
    if (nearBottom)
    {
        return ResizeEdge::Bottom;
    }
    return ResizeEdge::None;
}

void TitleBarLayer::handleTitleBarDoubleClick(const SDL_Event& event)
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

// Called every frame while a custom drag or resize is in progress. Reads the
// current global mouse state, computes the desired window position/size delta
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

    // Query the OS for the current pointer position and button mask.
    float globalMouseXF = 0.0F;
    float globalMouseYF = 0.0F;
    SDL_MouseButtonFlags mouseButtons = 0U;
    timedOp(m_TraceEnabled, mouseStateMs, [&] { mouseButtons = SDL_GetGlobalMouseState(&globalMouseXF, &globalMouseYF); });

    if ((mouseButtons & SDL_BUTTON_LMASK) == 0U)
    {
        endWindowInteraction();
        return;
    }

    const int globalMouseX = static_cast<int>(globalMouseXF);
    const int globalMouseY = static_cast<int>(globalMouseYF);
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
        const float xProportion =
            static_cast<float>(m_Drag.startMouseGlobalX - m_Drag.maximizedWindowX) / static_cast<float>(m_Drag.maximizedWindowWidth);
        timedOp(m_TraceEnabled, restoreMs, [&] { window.restore(); });
        const auto [restoredX, restoredY] = window.getPosition();
        const auto [restoredWidth, restoredHeight] = window.getSize();
        const int adjustedX = m_Drag.startMouseGlobalX - static_cast<int>(xProportion * static_cast<float>(restoredWidth));
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

    const auto [newX, newY, newWidth, newHeight] = computeResizeGeometry(
        m_Resize.edge, m_Resize.startWindowX, m_Resize.startWindowY, m_Resize.startWindowWidth, m_Resize.startWindowHeight, dx, dy);

    const bool positionChanged = (newX != m_Resize.lastAppliedX) || (newY != m_Resize.lastAppliedY);
    const bool sizeChanged = (newWidth != m_Resize.lastAppliedWidth) || (newHeight != m_Resize.lastAppliedHeight);
    const float now = Core::Application::getTime();
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
            timedOp(m_TraceEnabled, setSizeMs, [&] { SDL_SetWindowSize(window.getHandle(), newWidth, newHeight); });
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
        constexpr float MIN_RESIZE_EVENT_INTERVAL_SECONDS = 1.0F / 120.0F;
        const float resizeEventNow = Core::Application::getTime();
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
            SDL_SetWindowSize(sdlWindow, m_Resize.pendingWidth, m_Resize.pendingHeight);
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
    SDL_Window* sdlWindow = window.getHandle();
    if (sdlWindow == nullptr)
    {
        return;
    }

#ifndef _WIN32
    // On non-Windows the hit-test callback returns SDL_HITTEST_RESIZE_* for border
    // regions, so the window manager owns both the resize operation and its cursor.
    // When the WM grabs the pointer for a resize, SDL_GetMouseFocus() may stop
    // returning our window, which would incorrectly trigger a reset to the default
    // cursor and fight the WM-drawn resize cursor. Skip client-side cursor management
    // entirely on non-Windows — the WM keeps the correct resize cursor via hit-test.
    // (Client-side resize on Windows uses InteractionMode::Resize and is handled below.)
    (void) sdlWindow;
    return;
#endif

    // Seed from the cache so the state-unchanged fast-path still reaches the
    // cursor-apply step at the end (ImGui may reset the cursor each frame).
    const ResizeEdge prevEdge = m_CachedHoverEdge;
    ResizeEdge edge = m_CachedHoverEdge;

    if (m_InteractionMode == InteractionMode::Resize)
    {
        edge = m_Resize.edge;
    }
    else if (SDL_GetMouseFocus() != sdlWindow)
    {
        // Pointer is over another window (or has left ours) — no resize edge.
        edge = ResizeEdge::None;
        m_CachedHoverEdge = edge;
        m_HasCursorSample = false;
    }
    else
    {
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

            const bool insideWindow =
                (localX >= 0.0F && localY >= 0.0F && localX < static_cast<float>(windowWidth) && localY < static_cast<float>(windowHeight));
            edge = ResizeEdge::None;
            if (insideWindow)
            {
                edge = detectResizeEdge(localX, localY, windowWidth, windowHeight, isMaximized);
                // Suppress the resize cursor over title-bar controls: on Windows all
                // resize is client-side, but controls should still show the default cursor.
                if (edge != ResizeEdge::None && isPointInControlArea(localX, localY))
                {
                    edge = ResizeEdge::None;
                }
            }
            m_CachedHoverEdge = edge;
        }
        // State unchanged: edge == m_CachedHoverEdge, falls through to apply.
    }

    // Set a resize cursor while on a border. Restore the default only when
    // leaving one so ImGui-provided cursors (text inputs, splitters, etc.) are
    // not overridden while the pointer is away from window borders.
    if (edge != ResizeEdge::None)
    {
        applyCursorForEdge(edge);
    }
    else if (prevEdge != ResizeEdge::None)
    {
        applyCursorForEdge(ResizeEdge::None);
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
    const auto& scheme = UI::Theme::get().scheme();
    auto& window = Core::Application::get().getWindow();
    const auto [windowWidth, windowHeight] = window.getSize();

    const float titleBarHeight = height();
    // Set up window for title bar - no padding, no scrolling, fixed position
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth), titleBarHeight));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, scheme.titleBgActive);

    ImGui::Begin("##TitleBar", nullptr, flags);

    // Icon (left side) - clickable for system menu
    // Size: title bar height minus 2px border on top and bottom
    const float ICON_SIZE = titleBarHeight - 4.0F;
    const float centerY = titleBarHeight * 0.5F;
    const float iconY = centerY - (ICON_SIZE * 0.5F);
    constexpr float iconX = 8.0F;

    if (m_IconTexture.valid())
    {
        ImGui::SetCursorPos(ImVec2(iconX, iconY));

        // Make icon clickable with invisible button.
        // We use titleBgActive with zero alpha rather than a literal ImVec4(0,0,0,0) so that
        // if ImGui ever composites the RGB channel even at alpha=0, we still blend with
        // the actual title bar background color rather than black.
        ImGui::PushStyleColor(ImGuiCol_Button, UI::withAlpha(scheme.titleBgActive, 0.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.tabHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.tabSelected);
        if (ImGui::InvisibleButton("##IconButton", ImVec2(ICON_SIZE, ICON_SIZE)))
        {
#ifdef _WIN32
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
                        PostMessage(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
                    }
                }
            }
#else
            // Use custom system menu on Linux
            m_ShowSystemMenu = true;
#endif
        }
        ImGui::PopStyleColor(3);

        // Draw icon over the invisible button
        ImGui::SetCursorPos(ImVec2(iconX, iconY));
        ImGui::Image(m_IconTexture.textureId(), ImVec2(ICON_SIZE, ICON_SIZE));

        // Track icon bounds for right-click detection
        m_IconBounds = {.minX = iconX, .maxX = iconX + ICON_SIZE, .minY = iconY, .maxY = iconY + ICON_SIZE};
    }

    // Title text using Sixtyfour font - centered vertically
    ImGui::SameLine();
    ImGui::SetCursorPosX(8 + ICON_SIZE + 12);

    // Get the font to use and center the text vertically
    ImFont* titleFont = UI::Theme::get().titleFont();
    if (titleFont != nullptr)
    {
        ImGui::PushFont(titleFont);
    }

    // Now get the font size after pushing (ImGui::GetFontSize() returns current font size)
    const float fontSize = ImGui::GetFontSize();
    const float titleY = centerY - (fontSize * 0.5F);
    ImGui::SetCursorPosY(titleY);

    ImGui::TextColored(scheme.textPrimary, "TaskSmack");

    if (titleFont != nullptr)
    {
        ImGui::PopFont();
    }

    // Right side buttons
    constexpr float BUTTON_WIDTH = 46.0F;
    const float BUTTON_HEIGHT = titleBarHeight;
    const auto rightX = static_cast<float>(windowWidth);

    // Window control buttons (right to left: Close, Maximize, Minimize)
    // titleBgActive with zero alpha gives a transparent resting state; if ImGui ever composites
    // the RGB channel at alpha=0, we blend against the actual title bar background color.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, UI::withAlpha(scheme.titleBgActive, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.buttonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.buttonActive);

    // Close button (hover/active colors from theme)
    float buttonX = rightX - BUTTON_WIDTH;
    ImGui::SetCursorPos(ImVec2(buttonX, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.closeButtonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.closeButtonActive);
    if (ImGui::Button(ICON_FA_XMARK "##Close", ImVec2(BUTTON_WIDTH, BUTTON_HEIGHT)))
    {
        window.requestClose();
    }
    ImGui::PopStyleColor(2);
    m_CloseBounds = {.minX = buttonX, .maxX = buttonX + BUTTON_WIDTH, .minY = 0, .maxY = BUTTON_HEIGHT};

    // Maximize/Restore button
    buttonX -= BUTTON_WIDTH;
    ImGui::SetCursorPos(ImVec2(buttonX, 0));
    const bool isMaximized = window.isMaximized();
    if (ImGui::Button(isMaximized ? ICON_FA_WINDOW_RESTORE "##Restore" : ICON_FA_WINDOW_MAXIMIZE "##Maximize",
                      ImVec2(BUTTON_WIDTH, BUTTON_HEIGHT)))
    {
        if (isMaximized)
        {
            window.restore();
        }
        else
        {
            window.maximize();
        }
    }
    m_MaximizeBounds = {.minX = buttonX, .maxX = buttonX + BUTTON_WIDTH, .minY = 0, .maxY = BUTTON_HEIGHT};

    // Minimize button
    buttonX -= BUTTON_WIDTH;
    ImGui::SetCursorPos(ImVec2(buttonX, 0));
    if (ImGui::Button(ICON_FA_WINDOW_MINIMIZE "##Minimize", ImVec2(BUTTON_WIDTH, BUTTON_HEIGHT)))
    {
        window.minimize();
    }
    m_MinimizeBounds = {.minX = buttonX, .maxX = buttonX + BUTTON_WIDTH, .minY = 0, .maxY = BUTTON_HEIGHT};

    // Separator
    buttonX -= 16.0F;

    // Settings button
    buttonX -= BUTTON_WIDTH;
    ImGui::SetCursorPos(ImVec2(buttonX, 0));
    if (ImGui::Button(ICON_FA_GEAR "##Settings", ImVec2(BUTTON_WIDTH, BUTTON_HEIGHT)))
    {
        Core::OpenSettingsEvent event;
        Core::Application::get().raiseEvent(event);
    }
    m_SettingsBounds = {.minX = buttonX, .maxX = buttonX + BUTTON_WIDTH, .minY = 0, .maxY = BUTTON_HEIGHT};

    // Help button
    buttonX -= BUTTON_WIDTH;
    ImGui::SetCursorPos(ImVec2(buttonX, 0));
    if (ImGui::Button(ICON_FA_CIRCLE_QUESTION "##Help", ImVec2(BUTTON_WIDTH, BUTTON_HEIGHT)))
    {
        Core::OpenAboutEvent event;
        Core::Application::get().raiseEvent(event);
    }
    m_HelpBounds = {.minX = buttonX, .maxX = buttonX + BUTTON_WIDTH, .minY = 0, .maxY = BUTTON_HEIGHT};

    ImGui::PopStyleColor(3); // Button colors
    ImGui::PopStyleVar(2);   // Frame padding, item spacing

    // Alt+Space is handled in onSDLEvent() for reliable capture

    // Handle system menu popup (must be within the window context)
    if (m_ShowSystemMenu)
    {
        ImGui::OpenPopup("##SystemMenu");
        m_ShowSystemMenu = false;
    }

    // Render the system menu popup
    renderSystemMenu();

    ImGui::End();

    if (!window.isMaximized())
    {
        constexpr float BORDER_THICKNESS = 1.0F;
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        drawList->AddRect(ImVec2(0.0F, 0.0F),
                          ImVec2(static_cast<float>(windowWidth), static_cast<float>(windowHeight)),
                          ImGui::ColorConvertFloat4ToU32(scheme.border),
                          0.0F,
                          ImDrawFlags_None,
                          BORDER_THICKNESS);
    }

    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding, WindowBorderSize
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) - Intentionally non-static for OOP consistency
void TitleBarLayer::renderSystemMenu()
{
    auto& window = Core::Application::get().getWindow();
    const bool isMaximized = window.isMaximized();

    // Set position for the popup (below the icon)
    const float titleBarHeight = height();
    ImGui::SetNextWindowPos(ImVec2(8.0F, titleBarHeight), ImGuiCond_Appearing);

    if (ImGui::BeginPopup("##SystemMenu"))
    {
        // Restore (only enabled when maximized)
        if (isMaximized)
        {
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Restore"))
            {
                window.restore();
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Restore");
            ImGui::EndDisabled();
        }

        // Move (disabled when maximized)
        if (isMaximized)
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move");
            ImGui::EndDisabled();
        }
        else
        {
            // Move mode not implemented - would require special hit test mode
            static_cast<void>(ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move"));
        }

        // Size (disabled when maximized)
        if (isMaximized)
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_EXPAND "  Size");
            ImGui::EndDisabled();
        }
        else
        {
            // Size mode not implemented - would require special hit test mode
            static_cast<void>(ImGui::MenuItem(ICON_FA_EXPAND "  Size"));
        }

        // Minimize
        if (ImGui::MenuItem(ICON_FA_WINDOW_MINIMIZE "  Minimize"))
        {
            window.minimize();
        }

        // Maximize (only enabled when not maximized)
        if (!isMaximized)
        {
            if (ImGui::MenuItem(ICON_FA_WINDOW_MAXIMIZE "  Maximize"))
            {
                window.maximize();
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_WINDOW_MAXIMIZE "  Maximize");
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        // Close with shortcut hint
        if (ImGui::MenuItem(ICON_FA_XMARK "  Close", "Alt+F4"))
        {
            window.requestClose();
        }

        ImGui::EndPopup();
    }
}

} // namespace App
