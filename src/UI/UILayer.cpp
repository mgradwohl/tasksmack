#include "UILayer.h"

#include "Core/Application.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/ResizePerfOperation.h"
#include "Core/WindowEvents.h"
#include "UI/AssetPath.h"
#include "UI/ChartWidgets.h"
#include "UI/DpiScale.h"
#include "UI/FontFileCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/MonospaceFontPath.h"
#include "UI/RenderMetrics.h"
#include "UI/TabularDigits.h"
#include "UI/Theme.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <system_error>

namespace
{
// The main window's UI scale in window units (1.0 at 96 DPI): SDL's display scale over the window's
// pixel density, which ImGui already renders at (#1096). 1.0 without a window, and 0.0 if SDL fails
// to report the display scale (callers reject that through UI::displayScaleChanged()).
/// ImGui asserts on font data of this many bytes or fewer (AddFontFromMemoryTTF's sanity check).
constexpr std::size_t MIN_FONT_DATA_BYTES = 100;

/// AddFontFromFileTTF(), but from the bytes in @p fontFiles (#1170): each file is read once and every
/// font made from it shares that buffer, which the cache keeps alive for the atlas
/// (FontDataOwnedByAtlas = false). Like AddFontFromFileTTF() with ImFontFlags_NoLoadError, a file
/// that cannot be read adds nothing and returns nullptr.
ImFont* addFontFromFile(UI::FontFileCache& fontFiles,
                        const std::filesystem::path& path,
                        float sizePixels,
                        const ImFontConfig* configTemplate,
                        const ImWchar* glyphRanges = nullptr)
{
    const std::span<std::byte> data = fontFiles.get(path);
    if (data.size() <= MIN_FONT_DATA_BYTES || data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return nullptr;
    }
    ImFontConfig config = configTemplate != nullptr ? *configTemplate : ImFontConfig{};
    config.FontDataOwnedByAtlas = false;
    if (config.Name[0] == '\0')
    {
        // The file name, as AddFontFromFileTTF() names its fonts (shown in ImGui's Metrics window).
        const std::string name = path.filename().string();
        const std::span<char> dest(config.Name);
        const std::size_t length = std::min(name.size(), dest.size() - 1);
        std::copy_n(name.begin(), length, dest.begin());
        dest[length] = '\0';
    }
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()), sizePixels, &config, glyphRanges);
}

/// A body font with tabular digits (#1201): a first source that supplies only '0'-'9', each advanced
/// by the widest digit's width (UI/TabularDigits.h), then the whole font merged after it for every
/// other glyph. Without a digit width (FreeType couldn't measure the file) it is the plain font.
/// Like addFontFromFile(), nullptr if the file can't be read.
ImFont* addBodyFont(UI::FontFileCache& fontFiles, const std::filesystem::path& path, float sizePixels, std::optional<float> digitRatio)
{
    const float digitAdvance = digitRatio.has_value() ? UI::tabularDigitAdvancePx(*digitRatio, sizePixels) : 0.0F;
    if (digitAdvance > 0.0F)
    {
        ImFontConfig digitsConfig;
        digitsConfig.Flags = ImFontFlags_NoLoadError;
        digitsConfig.GlyphExcludeRanges = UI::NON_DIGIT_GLYPH_RANGES.data();
        digitsConfig.GlyphMinAdvanceX = digitAdvance;
        ImFont* font = addFontFromFile(fontFiles, path, sizePixels, &digitsConfig);
        if (font != nullptr)
        {
            ImFontConfig restConfig;
            restConfig.Flags = ImFontFlags_NoLoadError;
            restConfig.MergeMode = true;
            // The same bytes the digits were just read from, so this does not fail where that worked.
            (void) addFontFromFile(fontFiles, path, sizePixels, &restConfig);
            return font;
        }
    }
    ImFontConfig config;
    config.Flags = ImFontFlags_NoLoadError;
    return addFontFromFile(fontFiles, path, sizePixels, &config);
}

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

void UILayer::loadAllFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale)
{
    // Every size below is converted at this one measured scale -- the same value Theme scales the
    // style by -- so the fonts and the chrome around them always agree (#943).
    const auto pointsToPixels = [displayScale](float points)
    {
        return computePointsToPixels(points, displayScale);
    };

    auto& theme = Theme::get();
    ImGuiIO& imguiIO = ImGui::GetIO();

    // Configure FreeType for better hinting at small sizes
    // Note: IMGUI_ENABLE_FREETYPE is defined at compile time, so FreeType is always used
    // LightHinting provides better quality for UI fonts at typical screen sizes
    imguiIO.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;

    auto fontPath = (assetsDir / "fonts" / "Inter-Regular.ttf").string();
    auto iconFontPath = (assetsDir / "fonts" / FONT_ICON_FILE_NAME_FAS).string();
    const auto monospaceFontPath = findMonospaceFontPath();

    // Check if icon font exists. The error_code overloads here and below: this also runs when the
    // display scale changes (#943), after the old fonts are gone, and a filesystem error must
    // degrade to "not found" rather than throw out of the rebuild. Every font below is added from
    // fontFiles, which reads each file once and keeps it for later rebuilds (#1170), so a file that
    // cannot be read just returns nullptr to the fallbacks here. ImFontFlags_NoLoadError still
    // covers data FreeType rejects: without it ImGui asserts instead of returning nullptr.
    std::error_code existsError;
    const bool hasIconFont = std::filesystem::exists(iconFontPath, existsError);
    if (!hasIconFont)
    {
        spdlog::warn("Icon font not found at {}, icons will not be available", iconFontPath);
    }
    else
    {
        spdlog::info("Found icon font: {}", iconFontPath);
    }

    // Icon font glyph range (Font Awesome 6)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - ImGui API requires null-terminated C array for the glyph ranges
    static constexpr ImWchar ICON_RANGES[] = {ICON_MIN_FA, ICON_MAX_FA, 0};

    spdlog::info("Pre-baking fonts for all {} size presets with FreeType renderer", FONT_SIZE_COUNT);

    // Inter's widest digit, so every digit can be given its width (#1201). Read from the bytes the
    // atlas is built from; measured once for all the presets.
    const std::optional<float> digitRatio = widestDigitAdvanceRatio(fontFiles.get(fontPath));
    if (!digitRatio.has_value())
    {
        spdlog::warn("Could not measure the digits of {}; numbers will use proportional digits", fontPath);
    }

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

        ImFont* fontRegular = addBodyFont(fontFiles, fontPath, fontSizeRegular, digitRatio);
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
            iconConfig.Flags |= ImFontFlags_NoLoadError;
            iconConfig.MergeMode = true;
            iconConfig.PixelSnapH = true;
            iconConfig.GlyphMinAdvanceX = fontSizeRegular; // Make icons monospaced
            addFontFromFile(fontFiles, iconFontPath, fontSizeRegular, &iconConfig, ICON_RANGES);
        }

        ImFont* fontLarge = addBodyFont(fontFiles, fontPath, fontSizeLarge, digitRatio);
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
            iconConfig.Flags |= ImFontFlags_NoLoadError;
            iconConfig.MergeMode = true;
            iconConfig.PixelSnapH = true;
            iconConfig.GlyphMinAdvanceX = fontSizeLarge;
            addFontFromFile(fontFiles, iconFontPath, fontSizeLarge, &iconConfig, ICON_RANGES);
        }

        ImFont* fontMonospace = nullptr;
        if (!monospaceFontPath.empty())
        {
            ImFontConfig monoConfig;
            monoConfig.Flags |= ImFontFlags_NoLoadError;
            monoConfig.FontLoaderFlags |= ImGuiFreeTypeBuilderFlags_MonoHinting;
            monoConfig.SizePixels = fontSizeRegular;
            fontMonospace = addFontFromFile(fontFiles, monospaceFontPath, fontSizeRegular, &monoConfig);
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

    // The title bar's two physical sizes. Both are given in points and converted against the
    // measured display scale, like every other font here, so the bar tracks display density; and
    // both deliberately ignore the Font Size setting, because the title bar is chrome, not content.
    //
    // They are independent of each other on purpose. The bar's height used to be derived from the
    // title font plus padding, which is what made the bar, its icon and its window buttons balloon
    // whenever a larger body font was picked, so nothing below may reintroduce that coupling: the
    // wordmark is sized to look right as a wordmark, and the bar is sized to be a title bar.
    //
    // TITLE_FONT_PT is rounded to a whole pixel because the Sixtyfour face is rasterized as a
    // bitmap (ImGuiFreeTypeBuilderFlags_Bitmap below); a fractional size would render it soft.
    constexpr float TITLE_FONT_PT = 18.0F;
    const float titleFontPx = std::round(pointsToPixels(TITLE_FONT_PT));

    // Everything drawn in the bar -- the application icon, the window and app buttons, and their
    // glyphs -- is derived from TITLE_BAR_PT, so they all scale together with it.
    const float titleBarPx = computeTitleBarHeightPx(displayScale);
    theme.setTitleBarHeightPx(titleBarPx);
    spdlog::info("Title bar {}pt -> {}px (title font {}pt -> {}px)", TITLE_BAR_PT, titleBarPx, TITLE_FONT_PT, titleFontPx);
    auto titleFontPath = (assetsDir / "fonts" / "Sixtyfour.ttf").string();
    if (std::filesystem::exists(titleFontPath, existsError))
    {
        ImFontConfig titleConfig;
        titleConfig.Flags |= ImFontFlags_NoLoadError;
        titleConfig.FontLoaderFlags |= ImGuiFreeTypeBuilderFlags_Bitmap;
        ImFont* titleFont = addFontFromFile(fontFiles, titleFontPath, titleFontPx, &titleConfig);
        if (titleFont != nullptr)
        {
            theme.registerTitleFont(titleFont);
            spdlog::info("Loaded Sixtyfour title font at {}pt -> {}px", TITLE_FONT_PT, titleFontPx);
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

    // Chrome icon font: Font Awesome at a fixed size for the title bar's window and app controls.
    //
    // The icon glyphs are otherwise merged into each body font at that font's size, so the controls
    // drawn under the globally pushed Theme::regularFont() grew and shrank with the Font Size
    // setting even once their boxes were fixed -- a 6x difference in rendered glyph area between the
    // Small and Even Huger presets. Sizing it from the bar keeps the glyphs proportional to the bar
    // they sit in, and independent of the body font like the rest of the chrome.
    //
    // The ratio is set against the rest of the bar's contents rather than picked for looks: the
    // application icon occupies 94% of the bar height and the "TaskSmack" wordmark about 66%, so
    // controls at the 41% these glyphs previously rendered at read as a different, smaller family of
    // chrome. 0.55 puts them at roughly the wordmark's cap height with even clearance above and
    // below, and still well inside the TITLE_BAR_BUTTON_ASPECT-wide button box.
    if (hasIconFont)
    {
        const float chromeIconPx = computeChromeIconPx(titleBarPx);
        ImFontConfig chromeConfig;
        chromeConfig.Flags |= ImFontFlags_NoLoadError;
        chromeConfig.PixelSnapH = true;
        chromeConfig.GlyphMinAdvanceX = chromeIconPx; // keep the controls monospaced
        ImFont* chromeIconFont = addFontFromFile(fontFiles, iconFontPath, chromeIconPx, &chromeConfig, ICON_RANGES);
        if (chromeIconFont != nullptr)
        {
            theme.registerChromeIconFont(chromeIconFont, chromeIconPx);
            spdlog::info("Loaded chrome icon font at {}px", chromeIconPx);
        }
    }

    spdlog::info("Pre-baked {} fonts into atlas using FreeType, from {} font files", imguiIO.Fonts->Fonts.Size, fontFiles.fileCount());
}

void UILayer::loadFallbackFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale)
{
    // ImGui's embedded font at each preset's sizes: no font files needed, so it cannot fail the way
    // loadAllFonts() can. The Sixtyfour wordmark stays unregistered, which the title bar already
    // handles.
    //
    // The title bar is still resized to the new scale, and Font Awesome is still merged in when it
    // can be read: without them a failed rebuild kept the old scale's bar height, and every
    // ICON_FA_* label (the tabs among them) drew as missing-glyph boxes (#1169). The icon font is
    // optional here -- ImFontFlags_NoLoadError makes an unreadable file add nothing.
    auto& theme = Theme::get();
    const ImGuiIO& imguiIO = ImGui::GetIO();
    const float titleBarPx = computeTitleBarHeightPx(displayScale);
    theme.setTitleBarHeightPx(titleBarPx);

    const auto iconFontPath = (assetsDir / "fonts" / FONT_ICON_FILE_NAME_FAS).string();
    std::error_code existsError;
    const bool hasIconFont = !assetsDir.empty() && std::filesystem::exists(iconFontPath, existsError);
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - ImGui API requires a null-terminated C array
    static constexpr ImWchar ICON_RANGES[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    const auto mergeIcons = [&](float sizePx)
    {
        if (!hasIconFont)
        {
            return;
        }
        ImFontConfig iconConfig;
        iconConfig.Flags = ImFontFlags_NoLoadError;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = sizePx;
        addFontFromFile(fontFiles, iconFontPath, sizePx, &iconConfig, ICON_RANGES);
    };

    for (const auto size : ALL_FONT_SIZES)
    {
        const auto& fontCfg = theme.fontConfig(size);
        ImFontConfig regularConfig;
        regularConfig.SizePixels = computePointsToPixels(fontCfg.regularPt, displayScale);
        ImFont* regular = imguiIO.Fonts->AddFontDefault(&regularConfig);
        mergeIcons(regularConfig.SizePixels);
        ImFontConfig largeConfig;
        largeConfig.SizePixels = computePointsToPixels(fontCfg.largePt, displayScale);
        ImFont* large = imguiIO.Fonts->AddFontDefault(&largeConfig);
        mergeIcons(largeConfig.SizePixels);
        theme.registerFonts(size, regular, large, regular);
    }

    if (hasIconFont)
    {
        const float chromeIconPx = computeChromeIconPx(titleBarPx);
        ImFontConfig chromeConfig;
        chromeConfig.Flags = ImFontFlags_NoLoadError;
        chromeConfig.PixelSnapH = true;
        chromeConfig.GlyphMinAdvanceX = chromeIconPx;
        if (ImFont* chromeIconFont = addFontFromFile(fontFiles, iconFontPath, chromeIconPx, &chromeConfig, ICON_RANGES);
            chromeIconFont != nullptr)
        {
            theme.registerChromeIconFont(chromeIconFont, chromeIconPx);
        }
    }
}

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
        // imguiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Multi-viewport (optional)

        // Disable ImGui's default INI file - we store layout state in TOML config
        imguiIO.IniFilename = nullptr;

        // Measure the display density once and give the same value to Theme (which scales the
        // ImGuiStyle by it, #936) and to the fonts, so the two cannot disagree. A failed
        // measurement (0.0) leaves Theme at 1.0 and bakes the fonts at 1.0 to match.
        const float measuredScale = measureDisplayScale();
        Theme::get().setDisplayScale(measuredScale);
        const float displayScale = Theme::get().displayScale();

        // Pre-bake fonts for all size presets
        // Locate assets directory once (searches build dir and FHS install paths)
        m_AssetsDir = findAssetsDir();
        const auto& assetsDir = m_AssetsDir;
        loadAllFonts(m_FontFiles, assetsDir, displayScale);

        // Load themes from TOML files (built-ins)
        auto themesDir = assetsDir / "themes";
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
            spdlog::warn("Skipping user themes in {}: {}", userThemesDir.string(), ec.message());
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

    // Dragging the window to a differently scaled monitor, or changing the display's scale setting
    // (#943). Only noted here: the fonts and style are rebuilt at the next frame boundary, in
    // beginFrame(), because events can arrive while a frame is being laid out.
    if (event->type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED || event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
    {
        m_DisplayScaleCheckPending = true;
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
    if (m_DisplayScaleCheckPending)
    {
        m_DisplayScaleCheckPending = false;
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

    // Push the current font - store pointer so endFrame() can pop without a
    // second Theme lookup.
    m_PushedFont = Theme::get().regularFont();
    if (m_PushedFont != nullptr)
    {
        ImGui::PushFont(m_PushedFont);
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
