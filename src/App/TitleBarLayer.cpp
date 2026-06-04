#include "TitleBarLayer.h"

#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Layer.h"
#include "Core/WindowEvents.h"
#include "UI/AssetPath.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <string_view>

#include <stb_image.h>

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
constexpr float RESIZE_SIZE_COMMIT_INTERVAL_SECONDS = 1.0F / 30.0F;
constexpr float RESIZE_PENDING_IDLE_FLUSH_SECONDS = 1.0F / 60.0F;

// Conditionally time an operation and accumulate the duration into accumMs.
// When traceEnabled is false the call reduces to a branch and a direct callable invocation.
template<typename Fn> void timedOp(bool traceEnabled, double& accumMs, Fn&& fn)
{
    if (!traceEnabled)
    {
        fn();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    accumMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Lightweight RAII scope guard — runs a callable on scope exit.
template<typename Fn> struct ScopeExit
{
    explicit ScopeExit(Fn fn) : m_fn(std::move(fn))
    {}
    // noexcept: exceptions must not escape destructors. The callables used here
    // (trace logging accumulations) are non-throwing; the try/catch is a safety
    // net that prevents std::terminate if that assumption is ever violated.
    ~ScopeExit() noexcept
    {
        try
        {
            m_fn();
        }
        catch (...)
        {}
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

  private:
    Fn m_fn;
};

// Hit-test callback behavior is platform dependent:
// - Windows: force NORMAL and use client-side drag/resize to avoid modal move/size redraw stalls.
// - Non-Windows: keep native draggable/resize hit-test behavior for compositor-friendly moves/resizes.
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

    const float x = static_cast<float>(area->x);
    const float y = static_cast<float>(area->y);
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

    if (isInsideBounds(x, y, layer->getIconBounds()) || isInsideBounds(x, y, layer->getHelpBounds()) ||
        isInsideBounds(x, y, layer->getSettingsBounds()) || isInsideBounds(x, y, layer->getMinimizeBounds()) ||
        isInsideBounds(x, y, layer->getMaximizeBounds()) || isInsideBounds(x, y, layer->getCloseBounds()))
    {
        return SDL_HITTEST_NORMAL;
    }

    return SDL_HITTEST_DRAGGABLE;
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
    const float rightX = static_cast<float>(windowWidth);
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
    auto iconPath = (UI::findAssetsDir() / "icons" / "tasksmack-32.png").string();
    int channels = 0;
    unsigned char* data = stbi_load(iconPath.c_str(), &m_IconWidth, &m_IconHeight, &channels, 4);
    if (data != nullptr)
    {
        glGenTextures(1, &m_IconTexture);
        if (m_IconTexture == 0U)
        {
            spdlog::error("Failed to create OpenGL texture for title bar icon (glGenTextures returned 0)");
            stbi_image_free(data);
            m_IconWidth = 0;
            m_IconHeight = 0;
        }
        else
        {
            glBindTexture(GL_TEXTURE_2D, m_IconTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_IconWidth, m_IconHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

            const GLenum error = glGetError();
            if (error != GL_NO_ERROR)
            {
                spdlog::error("OpenGL error {} while uploading title bar icon texture", static_cast<unsigned int>(error));
                glBindTexture(GL_TEXTURE_2D, 0); // Unbind before deletion
                glDeleteTextures(1, &m_IconTexture);
                m_IconTexture = 0U;
                m_IconWidth = 0;
                m_IconHeight = 0;
            }
            else
            {
                spdlog::info("Loaded title bar icon: {}x{}", m_IconWidth, m_IconHeight);
            }

            // Unbind texture to avoid dangling OpenGL state
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(data);
        }
    }
    else
    {
        spdlog::warn("Failed to load title bar icon from {}", iconPath);
    }

    m_TraceEnabled = isResizePerfTracingEnabled();

    // Set up hit test for window dragging
    setupHitTest();
    createSystemCursors();
}

void TitleBarLayer::onDetach()
{
    spdlog::info("TitleBarLayer detached");

    if (m_IconTexture != 0)
    {
        glDeleteTextures(1, &m_IconTexture);
        m_IconTexture = 0;
    }

    destroySystemCursors();

    // Remove hit test callback
    Core::Application::get().getWindow().setHitTestCallback(nullptr, nullptr);
}

void TitleBarLayer::onUpdate([[maybe_unused]] float deltaTime)
{
#ifdef _WIN32
    updateWindowInteraction();
#endif
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

#ifdef _WIN32
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
#endif
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
    float globalMouseXF = 0.0F;
    float globalMouseYF = 0.0F;
    SDL_GetGlobalMouseState(&globalMouseXF, &globalMouseYF);
    const int globalMouseX = static_cast<int>(globalMouseXF);
    const int globalMouseY = static_cast<int>(globalMouseYF);

    if (edge != ResizeEdge::None)
    {
        const auto [startX, startY] = window.getPosition();
        m_InteractionMode = InteractionMode::Resize;
        m_Resize.edge = edge;
        m_Resize.startMouseGlobalX = globalMouseX;
        m_Resize.startMouseGlobalY = globalMouseY;
        m_Resize.startWindowX = startX;
        m_Resize.startWindowY = startY;
        m_Resize.startWindowWidth = windowWidth;
        m_Resize.startWindowHeight = windowHeight;
        m_Resize.lastAppliedX = startX;
        m_Resize.lastAppliedY = startY;
        m_Resize.lastAppliedWidth = windowWidth;
        m_Resize.lastAppliedHeight = windowHeight;
        m_Resize.hasPendingCommit = false;
        m_Resize.pendingWidth = windowWidth;
        m_Resize.pendingHeight = windowHeight;
        m_Resize.lastSizeCommitTime = Core::Application::getTime();
        m_Resize.lastDesiredChangeTime = m_Resize.lastSizeCommitTime;
        return;
    }

    if (mouseY <= height())
    {
        if (isMaximized)
        {
            // Don't restore yet — defer restore until the pointer has actually moved
            // past the drag threshold so a bare click doesn't unmaximize the window.
            const auto [maxX, maxY] = window.getPosition();
            m_Drag.pendingRestore = true;
            m_Drag.maximizedWindowX = maxX;
            m_Drag.maximizedWindowWidth = windowWidth;
            m_InteractionMode = InteractionMode::Drag;
            m_Drag.startMouseGlobalX = globalMouseX;
            m_Drag.startMouseGlobalY = globalMouseY;
            // Placeholder positions replaced once restore actually happens.
            m_Drag.startWindowX = maxX;
            m_Drag.startWindowY = maxY;
            m_Drag.lastAppliedX = maxX;
            m_Drag.lastAppliedY = maxY;
            m_Resize.edge = ResizeEdge::None;
            (void) maxY;
        }
        else
        {
            const auto [startX, startY] = window.getPosition();
            m_InteractionMode = InteractionMode::Drag;
            m_Drag.startMouseGlobalX = globalMouseX;
            m_Drag.startMouseGlobalY = globalMouseY;
            m_Drag.startWindowX = startX;
            m_Drag.startWindowY = startY;
            m_Drag.lastAppliedX = startX;
            m_Drag.lastAppliedY = startY;
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
            const char* mode = startedDrag ? "drag" : (startedResize ? "resize" : "none");
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
        m_Resize.lastAppliedX = newX;
        m_Resize.lastAppliedY = newY;
    }
    if (sizeChanged)
    {
        const bool commitIntervalElapsed = (now - m_Resize.lastSizeCommitTime) >= RESIZE_SIZE_COMMIT_INTERVAL_SECONDS;
        const bool skipDuplicateSize = (newWidth == m_Resize.lastAppliedWidth) && (newHeight == m_Resize.lastAppliedHeight);
        if (commitIntervalElapsed)
        {
            if (!skipDuplicateSize)
            {
                timedOp(m_TraceEnabled, setSizeMs, [&] { SDL_SetWindowSize(window.getHandle(), newWidth, newHeight); });
            }
            m_Resize.lastAppliedWidth = newWidth;
            m_Resize.lastAppliedHeight = newHeight;
            m_Resize.lastSizeCommitTime = now;
            m_Resize.lastDesiredChangeTime = now;
            m_Resize.hasPendingCommit = false;
            sizeCommitApplied = true;
        }
        else
        {
            m_Resize.hasPendingCommit = true;
            m_Resize.pendingWidth = newWidth;
            m_Resize.pendingHeight = newHeight;
            m_Resize.lastDesiredChangeTime = now;
        }
    }
    else if (m_Resize.hasPendingCommit)
    {
        const bool idleElapsed = (now - m_Resize.lastDesiredChangeTime) >= RESIZE_PENDING_IDLE_FLUSH_SECONDS;
        if (idleElapsed)
        {
            const bool skipDuplicatePending =
                (m_Resize.pendingWidth == m_Resize.lastAppliedWidth) && (m_Resize.pendingHeight == m_Resize.lastAppliedHeight);
            if (!skipDuplicatePending)
            {
                timedOp(m_TraceEnabled,
                        setSizeMs,
                        [&] { SDL_SetWindowSize(window.getHandle(), m_Resize.pendingWidth, m_Resize.pendingHeight); });
                m_Resize.lastAppliedWidth = m_Resize.pendingWidth;
                m_Resize.lastAppliedHeight = m_Resize.pendingHeight;
            }
            m_Resize.lastSizeCommitTime = now;
            m_Resize.hasPendingCommit = false;
            sizeCommitApplied = true;
        }
        else
        {
            return;
        }
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
    SDL_Cursor* cursor = m_DefaultCursor;

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

    // Seed from the cache so the state-unchanged fast-path still reaches the
    // cursor-apply step at the end (ImGui may reset the cursor each frame).
    const ResizeEdge prevEdge = m_CachedHoverEdge;
    ResizeEdge edge = m_CachedHoverEdge;

    if (m_InteractionMode == InteractionMode::Resize)
    {
        edge = m_Resize.edge;
    }
    else
    {
        float globalMouseXF = 0.0F;
        float globalMouseYF = 0.0F;
        SDL_GetGlobalMouseState(&globalMouseXF, &globalMouseYF);
        const int mouseGlobalX = static_cast<int>(globalMouseXF);
        const int mouseGlobalY = static_cast<int>(globalMouseYF);

        const auto [windowX, windowY] = window.getPosition();
        const auto [windowWidth, windowHeight] = window.getSize();
        const bool isMaximized = window.isMaximized();

        const bool stateUnchanged = m_HasCursorSample && mouseGlobalX == m_LastCursorMouseGlobalX &&
                                    mouseGlobalY == m_LastCursorMouseGlobalY && windowX == m_LastCursorWindowX &&
                                    windowY == m_LastCursorWindowY && windowWidth == m_LastCursorWindowWidth &&
                                    windowHeight == m_LastCursorWindowHeight && isMaximized == m_LastCursorWindowMaximized;

        if (!stateUnchanged)
        {
            m_LastCursorMouseGlobalX = mouseGlobalX;
            m_LastCursorMouseGlobalY = mouseGlobalY;
            m_LastCursorWindowX = windowX;
            m_LastCursorWindowY = windowY;
            m_LastCursorWindowWidth = windowWidth;
            m_LastCursorWindowHeight = windowHeight;
            m_LastCursorWindowMaximized = isMaximized;
            m_HasCursorSample = true;

            const float localX = globalMouseXF - static_cast<float>(windowX);
            const float localY = globalMouseYF - static_cast<float>(windowY);

            const bool insideWindow =
                (localX >= 0.0F && localY >= 0.0F && localX < static_cast<float>(windowWidth) && localY < static_cast<float>(windowHeight));
            edge = ResizeEdge::None;
            if (insideWindow)
            {
                edge = detectResizeEdge(localX, localY, windowWidth, windowHeight, isMaximized);
                // Suppress the resize cursor over title-bar controls on all platforms:
                // Windows routes these through the custom resize path, and on non-Windows
                // the hit-test callback returns NORMAL for control areas, so the cursor
                // must match the action regardless of platform.
                if (edge != ResizeEdge::None && isPointInControlArea(localX, localY))
                {
                    edge = ResizeEdge::None;
                }
#ifndef _WIN32
                // On non-Windows, hitTestCallback returns NORMAL for the top-edge
                // band right of helpBounds.minX (so the WM doesn't intercept clicks
                // on that region). Suppress the resize cursor there to match.
                if (edge == ResizeEdge::Top && m_HelpBounds.maxX > m_HelpBounds.minX && localX >= m_HelpBounds.minX)
                {
                    edge = ResizeEdge::None;
                }
#endif
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

    if (m_IconTexture != 0)
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
            SDL_PropertiesID props = SDL_GetWindowProperties(sdlWindow);
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
                    int cmd = TrackPopupMenu(systemMenu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
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
        // ImTextureID is the opaque texture handle type used by ImGui.
        // On this platform it's an unsigned integer type, so we use static_cast for clarity.
        // The backend handles the actual type conversion internally when rendering.
        ImGui::Image(static_cast<ImTextureID>(m_IconTexture), ImVec2(ICON_SIZE, ICON_SIZE));

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
    const float rightX = static_cast<float>(windowWidth);

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
