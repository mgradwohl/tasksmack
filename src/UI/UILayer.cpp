#include "UILayer.h"

#include "Core/Application.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/ResizePerfOperation.h"
#include "Core/WindowEvents.h"
#include "UI/AssetPath.h"
#include "UI/ChartWidgets.h"
#include "UI/DpiScale.h"
#include "UI/FontLoading.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/ThemeLoader.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <ratio>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
// The main window's UI scale in window units (1.0 at 96 DPI): SDL's display scale over the window's
// pixel density, which ImGui already renders at (#1096). 1.0 without a window, and 0.0 if SDL fails
// to report the display scale (callers reject that through UI::displayScaleChanged()).
float measureDisplayScale()
{
    SDL_Window* window = Core::Application::get().getWindow().getHandle();
    if (window == nullptr)
    {
        return 1.0F;
    }
    const float displayScale = SDL_GetWindowDisplayScale(window);
    const float pixelDensity = SDL_GetWindowPixelDensity(window);
    const float scale = UI::windowUnitScale(displayScale, pixelDensity);
    spdlog::debug("UI scale {:.3f} (SDL display scale {:.3f}, pixel density {:.3f})", scale, displayScale, pixelDensity);
    return scale;
}

} // namespace

namespace UI
{

UILayer::UILayer() : Layer("UILayer")
{}

UILayer::~UILayer() = default;

void UILayer::onAttach()
{
    spdlog::info("Initializing ImGui");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    // Everything past context creation can throw (font/theme file I/O below, and in principle
    // the backend Init calls), and Application::pushLayer() pops a layer whose onAttach() threw
    // without calling onDetach() on it (onDetach() assumes full initialization -- see #779). So
    // this function must be exception-safe on its own: unwind exactly what it managed to set up
    // before rethrowing, mirroring Core::Window's constructor. backendsInitialized tracks
    // whether the ImGui_Impl*_Init calls below succeeded, so the catch block only calls their
    // Shutdown counterparts when there's actually something to shut down.
    bool backendsInitialized = false;
    try
    {
        spdlog::info("ImGui FreeType backend enabled (IMGUI_ENABLE_FREETYPE)");

        ImGuiIO& imguiIO = ImGui::GetIO();
        imguiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // ImGuiConfigFlags_ViewportsEnable (multi-viewport) is never set, so there are no platform
        // windows to style or render.

        // Disable ImGui's default INI file - we store layout state in TOML config
        imguiIO.IniFilename = nullptr;

        // Measure the display density once and give the same value to Theme (which scales the
        // ImGuiStyle by it, #936) and to the fonts, so the two cannot disagree. A failed
        // measurement (0.0) leaves Theme at 1.0 and bakes the fonts at 1.0 to match.
        const float measuredScale = measureDisplayScale();
        Theme::get().setDisplayScale(measuredScale);
        const float displayScale = Theme::get().displayScale();
        // A window already minimized has no real size, so its pixel density may be wrong in a way
        // windowUnitScale() can't tell (#3150): the scale is measured again at the first frame after
        // the restore, and the fonts rebuilt if it differs.
        if (Core::Application::get().getWindow().isMinimized())
        {
            m_DisplayScaleCheck.request();
        }

        // Pre-bake fonts for all size presets
        // Locate assets directory once (searches build dir and FHS install paths)
        m_AssetsDir = findAssetsDir();
        const auto& assetsDir = m_AssetsDir;
        loadAllFonts(m_FontFiles, assetsDir, displayScale);

        // Load themes from TOML files (built-ins)
        const auto themesDir = assetsDir / "themes";
        Theme::get().loadThemes(themesDir);
        spdlog::info("Loaded {} themes", Theme::get().discoveredThemes().size());

        // Optional: load user-provided themes from the same directory as config.toml (../themes)
        // They are merged over the built-ins by id (the filename stem), so a user theme overrides a built-in
        // with the same id instead of hiding all of them (#1127). The error_code overload keeps an unreadable
        // directory from throwing out of onAttach.
        const auto userThemesDir = Core::Application::get().paths().userConfigDir() / "themes";
        std::error_code ec;
        if (std::filesystem::is_directory(userThemesDir, ec))
        {
            Theme::get().loadThemes(userThemesDir);
        }
        else if (ec && ec != std::errc::no_such_file_or_directory)
        {
            // An absent directory is normal and stays quiet; anything else (permissions, a
            // symlink loop) would otherwise drop the user's themes without a word.
            spdlog::warn("{}", ThemeLoader::userThemesSkippedMessage(userThemesDir, ec));
        }

        // Apply default/fallback theme colors (user config will override later)
        Theme::get().applyImGuiStyle();

        // Setup Platform/Renderer backends
        SDL_Window* window = Core::Application::get().getWindow().getHandle();
        // Both report failure by returning false, not by throwing. The renderer's fails when its own
        // OpenGL loader can't load OpenGL 3 (#1398); carrying on would crash on its first null function.
        if (!ImGui_ImplSDL3_InitForOpenGL(window, Core::Application::get().getWindow().getGLContext()))
        // LCOV_EXCL_START - fails only without a window or OpenGL 3, which Core::Window already refuses (#1398)
        {
            throw std::runtime_error("Failed to initialize the ImGui SDL3 platform backend");
        }
        // LCOV_EXCL_STOP
        if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
        // LCOV_EXCL_START - fails only without a window or OpenGL 3, which Core::Window already refuses (#1398)
        {
            ImGui_ImplSDL3_Shutdown();
            throw std::runtime_error("Failed to initialize the ImGui OpenGL 3 renderer backend");
        }
        // LCOV_EXCL_STOP
        backendsInitialized = true;

        // Seed the cached pixel size and set the initial OpenGL viewport.
        // All subsequent resize-driven glViewport calls happen in onEvent(WindowResizedEvent),
        // eliminating the per-frame SDL_GetWindowSizeInPixels() query that beginFrame() used.
        if (window != nullptr)
        {
            SDL_GetWindowSizeInPixels(window, &m_CachedPixelW, &m_CachedPixelH);
            if (m_CachedPixelW > 0 && m_CachedPixelH > 0)
            {
                Core::traceResizePerfVoid(Core::ResizePerfOperation::Viewport,
                                          m_CachedPixelW,
                                          m_CachedPixelH,
                                          [&] { glViewport(0, 0, m_CachedPixelW, m_CachedPixelH); });
            }
        }
    }
    catch (...)
    {
        if (backendsInitialized)
        {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
        }
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        throw;
    }

    spdlog::info("ImGui initialized successfully");
}

void UILayer::onDetach()
{
    spdlog::info("Shutting down ImGui");

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    // The fonts go with the context's atlas: Theme must not keep pointers to them, and caches keyed on
    // (font, fontGeneration()) must not take a new font at a reused address for an old one (#2163).
    Theme::get().clearFontRegistrations();
    ImGui::DestroyContext();
}

void UILayer::onRender()
{
    beginFrame();

    // Demo windows are now controlled via View menu in ShellLayer
    // UILayer just initializes ImGui frame - actual UI is in other layers
}

void UILayer::onPostRender()
{
    endFrame();
}

void UILayer::onSDLEvent(SDL_Event* event)
{
    // Pass SDL events to ImGui for input handling
    ImGui_ImplSDL3_ProcessEvent(event);

    // Dragging the window to a differently scaled monitor, or changing the display's scale setting
    // (#943), and a restore, which a check asked for while minimized waits for (#3150). Only noted
    // here: the fonts and style are rebuilt at the next frame boundary, in beginFrame(), because
    // events can arrive while a frame is being laid out.
    if (requestsDisplayScaleCheck(event->type))
    {
        m_DisplayScaleCheck.request();
    }
}

void UILayer::rebuildForDisplayScaleChange()
{
    const float measured = measureDisplayScale();
    if (!displayScaleChanged(Theme::get().displayScale(), measured))
    {
        return;
    }
    spdlog::info("Display scale changed from {:.2f} to {:.2f}; rebuilding fonts and style", Theme::get().displayScale(), measured);

    // Fonts first, then the style, both before NewFrame(): the text and the chrome around it move
    // together, as the font-size presets do. Nothing keeps an ImFont* across frames except Theme,
    // whose registrations are dropped before the atlas is cleared -- an optional font that fails to
    // reload is then null, not dangling -- and re-made by loadAllFonts(). Caches keyed on a font
    // also see Theme::fontGeneration() advance. With ImGuiBackendFlags_RendererHasTextures the
    // backend re-uploads the atlas texture on the next render.
    const auto rebuildStart = std::chrono::steady_clock::now();
    Theme::get().clearFontRegistrations();
    ImGui::GetIO().Fonts->ClearFonts();
    try
    {
        loadAllFonts(m_FontFiles, m_AssetsDir, measured);
    }
    catch (const std::exception& e)
    {
        // The old fonts are already gone, and beginFrame() must still reach NewFrame() with a font
        // to draw with, so fall back to ImGui's built-in font rather than let this escape.
        spdlog::error("Rebuilding fonts at display scale {:.2f} failed ({}); using the built-in font", measured, e.what());
        Theme::get().clearFontRegistrations();
        ImGui::GetIO().Fonts->ClearFonts();
        loadFallbackFonts(m_FontFiles, m_AssetsDir, measured);
    }
    // Timed so the cost of a scale change can be measured (#1170). Glyphs are rasterized lazily, so
    // this covers loading the faces, not baking them.
    spdlog::info("Fonts rebuilt in {:.1f} ms",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rebuildStart).count());
    // Queues the style rebuild; applyPendingStyleChanges() flushes it straight after this.
    Theme::get().setDisplayScale(measured);
}

void UILayer::onEvent(Core::Event& event)
{
    // Keep the OpenGL viewport in sync with the framebuffer without polling SDL every frame.
    // glViewport() is called exactly once per pixel-size change, driven by the event system.
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::WindowResizedEvent>(
        [this](Core::WindowResizedEvent& e)
        {
            const int w = e.getWidth();
            const int h = e.getHeight();
            if (w > 0 && h > 0)
            {
                m_CachedPixelW = w;
                m_CachedPixelH = h;
                Core::traceResizePerfVoid(Core::ResizePerfOperation::Viewport, w, h, [&] { glViewport(0, 0, w, h); });
            }
            return false; // Do not consume; other layers may need the resize notification
        });
}

void UILayer::beginFrame()
{
    // Not while minimized: the window has no real size then, and its pixel density would make the
    // scale about 5x the display's (#3150). The check waits for the restore.
    if (m_DisplayScaleCheck.pending() && m_DisplayScaleCheck.takeDue(Core::Application::get().getWindow().isMinimized()))
    {
        rebuildForDisplayScaleChange();
    }

    // Apply any pending theme change BEFORE starting the ImGui frame
    // This ensures all widgets rendered this frame use the new theme colors
    Theme::get().applyPendingStyleChanges();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // Let frame-keyed chart caches age and free memory even on frames that draw no charts (#1173).
    UI::Widgets::trimFrameCaches();

    // Push the current font at its preset size - store pointer so endFrame() can pop without a
    // second Theme lookup.
    m_PushedFont = Theme::get().pushRegularFont();

    // Viewport is kept up-to-date by onEvent(WindowResizedEvent) and seeded in
    // onAttach(); no per-frame SDL_GetWindowSizeInPixels() query is needed.

    // Clear screen with ImGui's window background color (follows theme)
    const ImVec4& bgColor = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    glClearColor(bgColor.x, bgColor.y, bgColor.z, bgColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
}

void UILayer::endFrame()
{
    // Pop the font pushed in beginFrame() using the cached pointer (avoids a
    // second Theme::regularFont() lookup on every frame).
    if (m_PushedFont != nullptr)
    {
        ImGui::PopFont();
        m_PushedFont = nullptr;
    }

    Core::traceResizePerfVoid(Core::ResizePerfOperation::ImGuiFinalize, 0, 0, [] { ImGui::Render(); });

    // Captured once, between ImGui::Render() and the next ImGui::NewFrame(), because both the
    // submit call and the draw-call accounting below need the same ImDrawData.
    ImDrawData* drawData = ImGui::GetDrawData();
    Core::traceResizePerfVoid(Core::ResizePerfOperation::OpenGLSubmit,
                              m_CachedPixelW,
                              m_CachedPixelH,
                              [drawData] { ImGui_ImplOpenGL3_RenderDrawData(drawData); });

    // Publish this frame's draw-call/command-list counts for the Render Metrics overlay. This is
    // the only place they can be sampled: ImDrawData is valid only between ImGui::Render() and the
    // next ImGui::NewFrame(), and the overlay itself draws during the build phase, where
    // GetDrawData() returns null -- which is why it reported 0 on every frame (see #907).
    // Kept outside the OpenGLSubmit timer above so that measurement stays the GL submission
    // alone: it is load-bearing evidence for #882 and must not absorb this bookkeeping.
    if (RenderMetrics::get().enabled() && (drawData != nullptr))
    {
        int drawCalls = 0;
        for (const ImDrawList* cmdList : drawData->CmdLists)
        {
            drawCalls += cmdList->CmdBuffer.Size;
        }
        RenderMetrics::get().recordFrameDrawData(drawCalls, drawData->CmdListsCount);
    }
}

} // namespace UI
