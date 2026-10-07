#pragma once

#include "Theme.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UI
{

/// Information about a discovered theme file
struct ThemeInfo
{
    std::string id;             ///< Theme identifier (filename without extension)
    std::string name;           ///< Display name from [meta] section
    std::string description;    ///< Description from [meta] section
    std::filesystem::path path; ///< Full path to the TOML file
};

/**
 * @brief Loads themes from TOML files
 *
 * ThemeLoader handles:
 * - Discovering theme files in the assets/themes/ directory
 * - Parsing TOML files into ColorScheme structs
 * - Converting hex color strings and arrays to ImVec4
 * - Validating theme files for required fields
 */
class ThemeLoader
{
  public:
    /**
     * @brief Discover all available theme files
     * @param themesDir Path to the themes directory
     * @return Vector of ThemeInfo for each valid theme file
     */
    static auto discoverThemes(const std::filesystem::path& themesDir) -> std::vector<ThemeInfo>;

    /**
     * @brief Load a theme from a TOML file
     * @param path Path to the theme TOML file
     * @return ColorScheme if successful, nullopt on error
     */
    static auto loadTheme(const std::filesystem::path& path) -> std::optional<ColorScheme>;

    /**
     * @brief Load a theme from TOML text, exactly as loadTheme() reads a file but with no file I/O
     * @param tomlText Contents of a theme file
     * @param sourceName Name used in log messages and parse errors (e.g. the file it came from)
     * @return ColorScheme if the text is valid TOML, nullopt on error
     *
     * The seam the theme fuzz target drives (tests/fuzz/fuzz_theme_loader.cpp).
     */
    static auto loadThemeFromString(std::string_view tomlText, std::string_view sourceName = "<string>") -> std::optional<ColorScheme>;

    /**
     * @brief Load theme metadata without full color data
     * @param path Path to the theme TOML file
     * @return ThemeInfo if valid, nullopt on error
     */
    static auto loadThemeInfo(const std::filesystem::path& path) -> std::optional<ThemeInfo>;

    /**
     * @brief Convert a hex color string to ImVec4
     * @param hex Hex string like "#FF4081" or "#FF408180" (with alpha)
     * @return ImVec4 with normalized RGBA values
     */
    static auto hexToImVec4(std::string_view hex) -> ImVec4;
};

} // namespace UI
