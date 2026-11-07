#include "FontLoading.h"

#include "Core/Utf8Path.h"
#include "UI/DpiScale.h"
#include "UI/FontFileCache.h"
#include "UI/FontPointSize.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/TabularDigits.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <imgui_freetype.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace
{
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
        const std::string name = Core::pathToUtf8(path.filename());
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

} // namespace

namespace UI
{

void loadAllFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale)
{
    // Every size below is converted at this one measured scale -- the same value Theme scales the
    // style by -- so the fonts and the chrome around them always agree (#943).
    const auto pointsToPixels = [displayScale](float points)
    {
        return computePointsToPixels(points, displayScale);
    };

    auto& theme = Theme::get();
    const ImGuiIO& imguiIO = ImGui::GetIO();

    // Configure FreeType for better hinting at small sizes
    // Note: IMGUI_ENABLE_FREETYPE is defined at compile time, so FreeType is always used
    // LightHinting provides better quality for UI fonts at typical screen sizes
    imguiIO.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;

    const auto fontPath = assetsDir / "fonts" / "Inter-Regular.ttf";
    const auto boldFontPath = assetsDir / "fonts" / "Inter-Bold.ttf";
    const auto iconFontPath = assetsDir / "fonts" / FONT_ICON_FILE_NAME_FAS;

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
        spdlog::warn("Icon font not found at {}, icons will not be available", Core::pathToUtf8(iconFontPath));
    }
    else
    {
        spdlog::info("Found icon font: {}", Core::pathToUtf8(iconFontPath));
    }

    // Icon font glyph range (Font Awesome 6)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - ImGui API requires null-terminated C array for the glyph ranges
    static constexpr ImWchar ICON_RANGES[] = {ICON_MIN_FA, ICON_MAX_FA, 0};

    // One set of fonts, at the Font size setting (#3375): a change of size rebuilds the atlas at the
    // next frame boundary (UILayer), as a change of display scale does.
    const int pointSize = theme.fontPointSize();
    const FontPointSizes fontCfg = fontPointSizesFor(pointSize);
    spdlog::info("Building fonts at {} pt with FreeType renderer", pointSize);

    // Inter's widest digit, so every digit can be given its width (#1201). Read from the bytes the
    // atlas is built from.
    const std::optional<float> digitRatio = widestDigitAdvanceRatio(fontFiles.get(fontPath));
    if (!digitRatio.has_value())
    {
        spdlog::warn("Could not measure the digits of {}; numbers will use proportional digits", Core::pathToUtf8(fontPath));
    }

    // Font Awesome merged into the font added just before, at its size, monospaced.
    const auto mergeIcons = [&](float sizePx)
    {
        if (!hasIconFont)
        {
            return;
        }
        ImFontConfig iconConfig;
        iconConfig.Flags |= ImFontFlags_NoLoadError;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = sizePx; // Make icons monospaced
        addFontFromFile(fontFiles, iconFontPath, sizePx, &iconConfig, ICON_RANGES);
    };

    // Inter at sizePx with the icons merged in, or ImGui's embedded font when Inter can't be read.
    const auto addInterWithIcons = [&](float sizePx)
    {
        ImFont* font = addBodyFont(fontFiles, fontPath, sizePx, digitRatio);
        if (font == nullptr)
        {
            spdlog::warn("Could not load Inter font from {}, using default", Core::pathToUtf8(fontPath));
            ImFontConfig defaultFontConfig;
            defaultFontConfig.SizePixels = sizePx;
            font = imguiIO.Fonts->AddFontDefault(&defaultFontConfig);
        }
        mergeIcons(sizePx);
        return font;
    };

    const float fontSizeRegular = pointsToPixels(fontCfg.regularPt);
    const float fontSizeLarge = pointsToPixels(fontCfg.largePt);
    spdlog::debug("Loading fonts: {}pt = {:.1f}px, {}pt = {:.1f}px, charts {}pt",
                  fontCfg.regularPt,
                  fontSizeRegular,
                  fontCfg.largePt,
                  fontSizeLarge,
                  fontCfg.chartPt);

    ImFont* fontRegular = addInterWithIcons(fontSizeRegular);
    ImFont* fontLarge = addInterWithIcons(fontSizeLarge);

    // Bold at the body size, for section headers (#1200): the same size as the text beneath them,
    // so a header row is exactly as tall as before and the layout around it is unchanged, but a
    // weight apart from it. With the icons merged in for the header's glyph; nothing else needs a
    // bold face, so no large bold is loaded. Optional: without the file Theme::boldFont() hands out
    // the regular font.
    ImFontConfig boldConfig;
    boldConfig.Flags = ImFontFlags_NoLoadError;
    ImFont* fontBold = addFontFromFile(fontFiles, boldFontPath, fontSizeRegular, &boldConfig);
    if (fontBold != nullptr)
    {
        mergeIcons(fontSizeRegular);
    }

    // Chart text, a little smaller than the body text above 8 pt (chartFontPointSize()). At the
    // body size it is the regular font itself, so nothing more is added.
    ImFont* fontChart = nullptr;
    if (chartFontPointSize(pointSize) != pointSize)
    {
        fontChart = addInterWithIcons(pointsToPixels(fontCfg.chartPt));
    }

    // Register with theme. The bold and chart fonts after the rest: registerFonts() resets them.
    theme.registerFonts(pointSize, fontRegular, fontLarge);
    theme.registerBoldFont(fontBold);
    theme.registerChartFont(fontChart);

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
    const auto titleFontPath = assetsDir / "fonts" / "Sixtyfour.ttf";
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
            spdlog::warn("Failed to load Sixtyfour title font from {}", Core::pathToUtf8(titleFontPath));
        }
    }
    else
    {
        spdlog::warn("Sixtyfour title font not found at {}", Core::pathToUtf8(titleFontPath));
    }

    // Chrome icon font: Font Awesome at a fixed size for the title bar's window and app controls.
    //
    // The icon glyphs are otherwise merged into each body font at that font's size, so the controls
    // drawn under the globally pushed Theme::regularFont() grew and shrank with the Font Size
    // setting even once their boxes were fixed -- a 6x difference in rendered glyph area between the
    // old Small and Even Huger sizes. Sizing it from the bar keeps the glyphs proportional to the bar
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

    spdlog::info("Built {} fonts into atlas using FreeType, from {} font files", imguiIO.Fonts->Fonts.Size, fontFiles.fileCount());
}

void loadFallbackFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale)
{
    // ImGui's embedded font at the Font size setting's sizes: no font files needed, so it cannot fail the way
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

    const auto iconFontPath = assetsDir / "fonts" / FONT_ICON_FILE_NAME_FAS;
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

    const int pointSize = theme.fontPointSize();
    const FontPointSizes fontCfg = fontPointSizesFor(pointSize);
    const auto addDefault = [&](float points)
    {
        ImFontConfig config;
        config.SizePixels = computePointsToPixels(points, displayScale);
        ImFont* font = imguiIO.Fonts->AddFontDefault(&config);
        mergeIcons(config.SizePixels);
        return font;
    };
    ImFont* regular = addDefault(fontCfg.regularPt);
    ImFont* large = addDefault(fontCfg.largePt);
    ImFont* chart = chartFontPointSize(pointSize) != pointSize ? addDefault(fontCfg.chartPt) : nullptr;
    theme.registerFonts(pointSize, regular, large);
    theme.registerChartFont(chart);

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

} // namespace UI
