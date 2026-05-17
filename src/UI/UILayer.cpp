#include "UILayer.h"

#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/WindowEvents.h"
#include "UI/AssetPath.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>

namespace
{
// Per-frame state shared between beginFrame() and endFrame().
// Both methods are static (UILayer is a singleton); these file-scope variables
// replace what would otherwise be instance members.
ImFont* g_PushedFont = nullptr; // font pushed in beginFrame, popped in endFrame
int g_CachedPixelW = 0;         // last known framebuffer width  (avoids redundant glViewport)
int g_CachedPixelH = 0;         // last known framebuffer height (avoids redundant glViewport)

// Convert typographic points to pixels based on display DPI
// Standard: 1 point = 1/72 inch, base DPI assumed 96 (Windows/Linux standard)
float pointsToPixels(float points)
{
    constexpr float BASE_DPI = 96.0F;

    // Get display scale from SDL
    float scale = 1.0F;
    SDL_Window* window = Core::Application::get().getWindow().getHandle();
    if (window != nullptr)
    {
        scale = SDL_GetWindowDisplayScale(window);
    }

    // pixels = points * (DPI / 72), where effective DPI = BASE_DPI * scale
    return points * (BASE_DPI * scale) / 72.0F;
}

// Best-effort system monospace font discovery (platform-specific, prefers widely available defaults)
std::filesystem::path getMonospaceFontPath()
{
#ifdef _WIN32
    // Prefer Consolas, fall back to Cascadia Mono if available
    constexpr std::array<const wchar_t*, 2> CANDIDATES = {
        L"C:/Windows/Fonts/consola.ttf",
        L"C:/Windows/Fonts/CascadiaMono.ttf",
    };
    for (const auto* candidate : CANDIDATES)
    {
        std::filesystem::path path(candidate);
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
#else
    // Prefer DejaVu Sans Mono, fall back to Liberation Mono
    constexpr std::array<const char*, 2> CANDIDATES = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    };
    for (const auto* candidate : CANDIDATES)
    {
        std::filesystem::path path(candidate);
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
#endif

    return {};
}
} // namespace

namespace UI
{

UILayer::UILayer() : Layer("UILayer")
{}

UILayer::~UILayer() = default;

void UILayer::loadAllFonts(const std::filesystem::path& assetsDir)
{
    auto& theme = Theme::get();
    ImGuiIO& imguiIO = ImGui::GetIO();

    // Configure FreeType for better hinting at small sizes
    // Note: IMGUI_ENABLE_FREETYPE is defined at compile time, so FreeType is always used
    // LightHinting provides better quality for UI fonts at typical screen sizes
    imguiIO.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;

    auto fontPath = (assetsDir / "fonts" / "Inter-Regular.ttf").string();
    auto iconFontPath = (assetsDir / "fonts" / FONT_ICON_FILE_NAME_FAS).string();
    const auto monospaceFontPath = getMonospaceFontPath();

    // Check if icon font exists
    const bool hasIconFont = std::filesystem::exists(iconFontPath);
    if (!hasIconFont)
    {
        spdlog::warn("Icon font not found at {}, icons will not be available", iconFontPath);
    }
    else
    {
        spdlog::info("Found icon font: {}", iconFontPath);
    }

    // Icon font glyph range (Font Awesome 6)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - ImGui API requires null-terminated C array for AddFontFromFileTTF
    static constexpr ImWchar ICON_RANGES[] = {ICON_MIN_FA, ICON_MAX_FA, 0};

    spdlog::info("Pre-baking fonts for all {} size presets with FreeType renderer", FONT_SIZE_COUNT);

    // Load fonts for all size presets into a single atlas
    for (const auto size : ALL_FONT_SIZES)
    {
        const auto& fontCfg = theme.fontConfig(size);

        const float fontSizeRegular = pointsToPixels(fontCfg.regularPt);
        const float fontSizeLarge = pointsToPixels(fontCfg.largePt);

        spdlog::debug("Loading {} fonts: {}pt = {:.1f}px, {}pt = {:.1f}px",
                      fontCfg.name,
                      fontCfg.regularPt,
                      fontSizeRegular,
                      fontCfg.largePt,
                      fontSizeLarge);

        ImFont* fontRegular = imguiIO.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSizeRegular);
        if (fontRegular == nullptr)
        {
            spdlog::warn("Could not load Inter font from {}, using default", fontPath);
            ImFontConfig defaultFontConfig;
            defaultFontConfig.SizePixels = fontSizeRegular;
            fontRegular = imguiIO.Fonts->AddFontDefault(&defaultFontConfig);
        }

        // Merge icon font into regular font
        if (hasIconFont)
        {
            ImFontConfig iconConfig;
            iconConfig.MergeMode = true;
            iconConfig.PixelSnapH = true;
            iconConfig.GlyphMinAdvanceX = fontSizeRegular; // Make icons monospaced
            imguiIO.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), fontSizeRegular, &iconConfig, ICON_RANGES);
        }

        ImFont* fontLarge = imguiIO.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSizeLarge);
        if (fontLarge == nullptr)
        {
            ImFontConfig defaultFontConfig;
            defaultFontConfig.SizePixels = fontSizeLarge;
            fontLarge = imguiIO.Fonts->AddFontDefault(&defaultFontConfig);
        }

        // Merge icon font into large font
        if (hasIconFont)
        {
            ImFontConfig iconConfig;
            iconConfig.MergeMode = true;
            iconConfig.PixelSnapH = true;
            iconConfig.GlyphMinAdvanceX = fontSizeLarge;
            imguiIO.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), fontSizeLarge, &iconConfig, ICON_RANGES);
        }

        ImFont* fontMonospace = nullptr;
        if (!monospaceFontPath.empty())
        {
            ImFontConfig monoConfig;
            monoConfig.FontLoaderFlags |= ImGuiFreeTypeBuilderFlags_MonoHinting;
            monoConfig.SizePixels = fontSizeRegular;
            fontMonospace = imguiIO.Fonts->AddFontFromFileTTF(monospaceFontPath.string().c_str(), fontSizeRegular, &monoConfig);
            if (fontMonospace == nullptr)
            {
                spdlog::warn("Could not load monospace font from {}, falling back to default", monospaceFontPath.string());
            }
        }

        if (fontMonospace == nullptr)
        {
            ImFontConfig monoFallbackConfig;
            monoFallbackConfig.FontLoaderFlags |= ImGuiFreeTypeBuilderFlags_MonoHinting;
            monoFallbackConfig.SizePixels = fontSizeRegular;
            fontMonospace = imguiIO.Fonts->AddFontDefault(&monoFallbackConfig);
        }

        // Register with theme for instant switching
        theme.registerFonts(size, fontRegular, fontLarge, fontMonospace);
    }

    // Load Sixtyfour pixel font for custom title bar
    // This is a fixed-size font that looks best at specific pixel sizes
    constexpr float TITLE_FONT_SIZE_PX = 18.0F;
    auto titleFontPath = (assetsDir / "fonts" / "Sixtyfour.ttf").string();
    if (std::filesystem::exists(titleFontPath))
    {
        ImFontConfig titleConfig;
        titleConfig.FontLoaderFlags |= ImGuiFreeTypeBuilderFlags_Bitmap;
        ImFont* titleFont = imguiIO.Fonts->AddFontFromFileTTF(titleFontPath.c_str(), TITLE_FONT_SIZE_PX, &titleConfig);
        if (titleFont != nullptr)
        {
            theme.registerTitleFont(titleFont);
            spdlog::info("Loaded Sixtyfour title font at {}px", TITLE_FONT_SIZE_PX);
        }
        else
        {
            spdlog::warn("Failed to load Sixtyfour title font from {}", titleFontPath);
        }
    }
    else
    {
        spdlog::warn("Sixtyfour title font not found at {}", titleFontPath);
    }

    spdlog::info("Pre-baked {} fonts into atlas using FreeType", imguiIO.Fonts->Fonts.Size);
}

void UILayer::onAttach()
{
    spdlog::info("Initializing ImGui");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    spdlog::info("ImGui FreeType backend enabled (IMGUI_ENABLE_FREETYPE)");

    ImGuiIO& imguiIO = ImGui::GetIO();
    imguiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // imguiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Multi-viewport (optional)

    // Disable ImGui's default INI file - we store layout state in TOML config
    imguiIO.IniFilename = nullptr;

    // Pre-bake fonts for all size presets
    // Locate assets directory once (searches build dir and FHS install paths)
    const auto assetsDir = findAssetsDir();
    loadAllFonts(assetsDir);

    // Load themes from TOML files (built-ins)
    auto themesDir = assetsDir / "themes";
    Theme::get().loadThemes(themesDir);
    spdlog::info("Loaded {} themes", Theme::get().discoveredThemes().size());

    // Optional: load user-provided themes from the same directory as config.toml (../themes)
    const auto userThemesDir = Core::Application::get().paths().userConfigDir() / "themes";
    if (std::filesystem::exists(userThemesDir))
    {
        Theme::get().loadThemes(userThemesDir);
    }

    // Apply default/fallback theme colors (user config will override later)
    Theme::get().applyImGuiStyle();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones
    // NOTE: This alpha override is required by ImGui for multi-viewport support - not a theme color
    ImGuiStyle& style = ImGui::GetStyle();
    if ((imguiIO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
    {
        style.WindowRounding = 0.0F;
        style.Colors[ImGuiCol_WindowBg].w = 1.0F; // NOLINT: Required by ImGui viewports
    }

    // Setup Platform/Renderer backends
    SDL_Window* window = Core::Application::get().getWindow().getHandle();
    ImGui_ImplSDL3_InitForOpenGL(window, Core::Application::get().getWindow().getGLContext());
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // Seed the cached pixel size and set the initial OpenGL viewport.
    // All subsequent resize-driven glViewport calls happen in onEvent(WindowResizedEvent),
    // eliminating the per-frame SDL_GetWindowSizeInPixels() query that beginFrame() used.
    if (window != nullptr)
    {
        SDL_GetWindowSizeInPixels(window, &g_CachedPixelW, &g_CachedPixelH);
        if (g_CachedPixelW > 0 && g_CachedPixelH > 0)
        {
            glViewport(0, 0, g_CachedPixelW, g_CachedPixelH);
        }
    }

    spdlog::info("ImGui initialized successfully");
}

void UILayer::onDetach()
{
    spdlog::info("Shutting down ImGui");

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

void UILayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No font rebuild needed - fonts are pre-baked at all sizes
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
}

void UILayer::onEvent(Core::Event& event)
{
    // Keep the OpenGL viewport in sync with the framebuffer without polling SDL every frame.
    // glViewport() is called exactly once per pixel-size change, driven by the event system.
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::WindowResizedEvent>(
        [](Core::WindowResizedEvent& e)
        {
            const int w = e.getWidth();
            const int h = e.getHeight();
            if (w > 0 && h > 0)
            {
                g_CachedPixelW = w;
                g_CachedPixelH = h;
                glViewport(0, 0, w, h);
            }
            return false; // Do not consume; other layers may need the resize notification
        });
}

void UILayer::beginFrame()
{
    // Apply any pending theme change BEFORE starting the ImGui frame
    // This ensures all widgets rendered this frame use the new theme colors
    Theme::get().applyPendingTheme();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Push the current font - store pointer so endFrame() can pop without a
    // second Theme lookup.
    g_PushedFont = Theme::get().regularFont();
    if (g_PushedFont != nullptr)
    {
        ImGui::PushFont(g_PushedFont);
    }

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
    if (g_PushedFont != nullptr)
    {
        ImGui::PopFont();
        g_PushedFont = nullptr;
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Handle multi-viewport
    const ImGuiIO& imguiIO = ImGui::GetIO();
    if ((imguiIO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
    {
        SDL_Window* backupWindow = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupContext = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backupWindow, backupContext);
    }
}

} // namespace UI
