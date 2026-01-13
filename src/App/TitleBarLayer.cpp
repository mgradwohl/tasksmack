#include "TitleBarLayer.h"

#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Layer.h"
#include "Platform/Factory.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

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

// Hit test callback for SDL - determines if a point is draggable
SDL_HitTestResult hitTestCallback(SDL_Window* sdlWindow, const SDL_Point* area, void* data)
{
    auto* layer = static_cast<TitleBarLayer*>(data);
    if (layer == nullptr || area == nullptr)
    {
        return SDL_HITTEST_NORMAL;
    }

    // SDL hit test coordinates are in LOGICAL (window) coordinates
    // ImGui also uses logical coordinates for positioning (SDL backend handles DPI)
    // Button bounds from renderTitleBar() are in logical coordinates
    // So we use coordinates WITHOUT scaling
    const auto x = static_cast<float>(area->x);
    const auto y = static_cast<float>(area->y);

    // Title bar height is in logical coordinates
    const float titleBarHeight = TitleBarLayer::height();

    // Get window size in LOGICAL coordinates for consistency
    int windowWidth = 0;
    int windowHeight = 0;
    if (!SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight))
    {
        spdlog::error("SDL_GetWindowSize failed in hitTestCallback: {}", SDL_GetError());
        return SDL_HITTEST_NORMAL;
    }

    // Check if window is maximized - no resize borders when maximized
    const bool isMaximized = ((SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_MAXIMIZED) != 0);

    // Check window resize edges first (8px border in logical coordinates)
    // Skip resize edge detection when maximized
    constexpr float resizeBorder = 8.0F;

    if (!isMaximized)
    {
        // Bottom corners and edges
        if (y >= static_cast<float>(windowHeight) - resizeBorder)
        {
            if (x < resizeBorder)
            {
                return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            }
            if (x >= static_cast<float>(windowWidth) - resizeBorder)
            {
                return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            }
            return SDL_HITTEST_RESIZE_BOTTOM;
        }

        // Side edges
        if (x < resizeBorder)
        {
            if (y < resizeBorder)
            {
                return SDL_HITTEST_RESIZE_TOPLEFT;
            }
            return SDL_HITTEST_RESIZE_LEFT;
        }
        if (x >= static_cast<float>(windowWidth) - resizeBorder)
        {
            if (y < resizeBorder)
            {
                return SDL_HITTEST_RESIZE_TOPRIGHT;
            }
            return SDL_HITTEST_RESIZE_RIGHT;
        }

        // Top edge (but only if not in button area)
        if (y < resizeBorder)
        {
            // Don't resize if clicking in the button area on the right
            const auto& helpBounds = layer->getHelpBounds();
            if (helpBounds.maxX > helpBounds.minX && x >= helpBounds.minX)
            {
                return SDL_HITTEST_NORMAL; // Let ImGui handle buttons
            }
            return SDL_HITTEST_RESIZE_TOP;
        }
    }

    // Not in title bar area - let ImGui handle it normally
    if (y > titleBarHeight)
    {
        return SDL_HITTEST_NORMAL;
    }

    // In title bar area - check if on any button or the icon
    if (isInsideBounds(x, y, layer->getIconBounds()) || isInsideBounds(x, y, layer->getHelpBounds()) ||
        isInsideBounds(x, y, layer->getSettingsBounds()) || isInsideBounds(x, y, layer->getMinimizeBounds()) ||
        isInsideBounds(x, y, layer->getMaximizeBounds()) || isInsideBounds(x, y, layer->getCloseBounds()))
    {
        return SDL_HITTEST_NORMAL; // Let ImGui handle button/icon clicks
    }

    // In title bar but not on a button - this area is draggable
    return SDL_HITTEST_DRAGGABLE;
}

} // namespace

TitleBarLayer::TitleBarLayer() : Layer("TitleBarLayer")
{
}

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
    auto exeDir = []
    {
        auto provider = Platform::makePathProvider();
        return provider->getExecutableDir();
    }();

    auto iconPath = (exeDir / "assets" / "icons" / "tasksmack-32.png").string();
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

    // Set up hit test for window dragging
    setupHitTest();
}

void TitleBarLayer::onDetach()
{
    spdlog::info("TitleBarLayer detached");

    if (m_IconTexture != 0)
    {
        glDeleteTextures(1, &m_IconTexture);
        m_IconTexture = 0;
    }

    // Remove hit test callback
    Core::Application::get().getWindow().setHitTestCallback(nullptr, nullptr);
}

void TitleBarLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No update logic needed
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

        // Make icon clickable with invisible button
        ImGui::PushStyleColor(ImGuiCol_Button, scheme.button);
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
    // Use theme colors for button hover (except close which is red)
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, scheme.button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.buttonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.buttonActive);

    // Close button (red hover/active colors for common UI pattern)
    float buttonX = rightX - BUTTON_WIDTH;
    ImGui::SetCursorPos(ImVec2(buttonX, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8F, 0.1F, 0.1F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9F, 0.2F, 0.2F, 1.0F));
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
            if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move"))
            {
                // Move mode not implemented - would require special hit test mode
            }
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
            if (ImGui::MenuItem(ICON_FA_EXPAND "  Size"))
            {
                // Size mode not implemented - would require special hit test mode
            }
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
