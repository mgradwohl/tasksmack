#pragma once

// Finding a Linux process's icon the freedesktop.org way (#1800), best effort: a process has no icon
// of its own, but the application it belongs to usually has a desktop entry (a .desktop file) whose
// Icon names one in the icon theme.
//
// 1. Desktop entries are indexed by the program their Exec runs (its file name), their
//    StartupWMClass and their desktop file ID, case-folded. A process is looked up by its
//    executable's file name, then by its name.
// 2. The Icon value is found in the "hicolor" theme, which every theme falls back to: the nearest
//    PNG size at or above the size asked for (else the largest below), then the pixmaps directory.
//    SVG icons are skipped -- TaskSmack has no SVG decoder -- as are themes other than hicolor.
//
// Pure std::filesystem and string handling, no POSIX calls, so the tests run it against a temporary
// directory tree on every platform (tests/Platform/test_DesktopEntryIcons.cpp). Reading the process's
// executable and decoding the PNG are LinuxProcessIconSource's.

#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform::DesktopEntryIcons
{

/// The theme every icon theme inherits from, and the only one searched.
inline constexpr std::string_view FALLBACK_THEME = "hicolor";

/// The fixed sizes hicolor's directories hold PNGs at, smallest first.
inline constexpr std::array<int, 11> THEME_SIZES{16, 22, 24, 32, 36, 48, 64, 72, 96, 128, 256};

/// The longest desktop file read; a real one is a few KiB.
inline constexpr std::uintmax_t MAX_DESKTOP_FILE_BYTES = std::uintmax_t{256} * 1024U;

/// Programs an Exec line may start that run something else: never index keys, or every Python
/// process would take the icon of the first application launched through python3.
inline constexpr std::array<std::string_view, 18> LAUNCHERS{"env",
                                                            "sh",
                                                            "bash",
                                                            "dash",
                                                            "python",
                                                            "python3",
                                                            "perl",
                                                            "ruby",
                                                            "java",
                                                            "node",
                                                            "gjs",
                                                            "flatpak",
                                                            "pkexec",
                                                            "sudo",
                                                            "wine",
                                                            "mono",
                                                            "dbus-launch",
                                                            "snap"};

/// What a desktop entry's [Desktop Entry] group says, as far as icons go.
struct DesktopEntry
{
    std::string icon;           ///< Icon: a theme icon name, or an absolute file path
    std::string exec;           ///< Exec: the command line it launches
    std::string tryExec;        ///< TryExec: the program, when given
    std::string startupWmClass; ///< StartupWMClass: the window class its windows carry
    bool hidden = false;        ///< Hidden=true: the entry was deleted
    bool application = true;    ///< Type=Application (or no Type)
};

/// @p text lower-cased, ASCII only (desktop file IDs and program names are ASCII in practice): every
/// byte of 0x80 and up is kept as it is, whatever the locale (#2566).
[[nodiscard]] inline std::string foldCase(std::string_view text)
{
    std::string folded(text);
    std::ranges::transform(folded, folded.begin(), Platform::foldAscii);
    return folded;
}

/// The part of @p path after its last '/', or all of it.
[[nodiscard]] constexpr std::string_view fileName(std::string_view path) noexcept
{
    if (const std::size_t slash = path.rfind('/'); slash != std::string_view::npos)
    {
        path.remove_prefix(slash + 1);
    }
    return path;
}

/// The [Desktop Entry] group of a desktop file's @p text; nullopt when it has none or no Icon.
/// Localised keys (Name[de]) and every other group are ignored.
[[nodiscard]] inline std::optional<DesktopEntry> parseDesktopEntry(std::string_view text)
{
    DesktopEntry entry;
    bool inMainGroup = false;
    bool sawMainGroup = false;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view line = Platform::trimAscii(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);

        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        if (line.front() == '[')
        {
            inMainGroup = (line == "[Desktop Entry]");
            sawMainGroup = sawMainGroup || inMainGroup;
            continue;
        }
        if (!inMainGroup)
        {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = Platform::trimAscii(line.substr(0, equals));
        const std::string_view value = Platform::trimAscii(line.substr(equals + 1));
        if (key == "Icon")
        {
            entry.icon = value;
        }
        else if (key == "Exec")
        {
            entry.exec = value;
        }
        else if (key == "TryExec")
        {
            entry.tryExec = value;
        }
        else if (key == "StartupWMClass")
        {
            entry.startupWmClass = value;
        }
        else if (key == "Hidden")
        {
            entry.hidden = (value == "true");
        }
        else if (key == "Type")
        {
            entry.application = (value == "Application");
        }
    }
    if (!sawMainGroup || entry.icon.empty())
    {
        return std::nullopt;
    }
    return entry;
}

/// The words of an Exec line, double quotes grouping (the spec's quoting, without its escapes).
[[nodiscard]] inline std::vector<std::string> splitExec(std::string_view exec)
{
    std::vector<std::string> words;
    std::string word;
    bool quoted = false;
    bool inWord = false;
    std::size_t index = 0;
    while (index < exec.size())
    {
        const char c = exec[index];
        if (c == '"')
        {
            quoted = !quoted;
            inWord = true;
        }
        else if (!quoted && (c == ' ' || c == '\t'))
        {
            if (inWord)
            {
                words.push_back(std::move(word));
                word.clear();
                inWord = false;
            }
        }
        else
        {
            word.push_back(c);
            inWord = true;
        }
        ++index;
    }
    if (inWord)
    {
        words.push_back(std::move(word));
    }
    return words;
}

/// The file names, case-folded, of the programs an Exec line runs, for the index: its first word
/// that isn't a VAR=value or a launcher (LAUNCHERS), and for `flatpak run`, its --command=. Empty
/// when there is none (`python3 app.py` keys on nothing rather than on python3).
[[nodiscard]] inline std::vector<std::string> execProgramKeys(std::string_view exec)
{
    constexpr std::string_view COMMAND_OPTION = "--command=";
    std::vector<std::string> keys;
    const std::vector<std::string> words = splitExec(exec);
    std::size_t index = 0;
    while (index < words.size())
    {
        const std::string_view word = words[index];
        ++index;
        if (word.starts_with('-') || word.contains('='))
        {
            continue; // env's VAR=value, or an option of env
        }
        const std::string program = foldCase(fileName(word));
        if (program.empty() || program.starts_with('%'))
        {
            break; // A field code: there was no program
        }
        if (program == "env")
        {
            continue;
        }
        if (program == "flatpak")
        {
            // `flatpak run --command=prog app.id`: the program is the --command=, if there is one; the
            // app ID is the desktop file's ID, which is indexed anyway.
            while (index < words.size())
            {
                const std::string_view option = words[index];
                ++index;
                if (option.starts_with(COMMAND_OPTION))
                {
                    keys.push_back(foldCase(fileName(option.substr(COMMAND_OPTION.size()))));
                    break;
                }
            }
            break;
        }
        if (std::ranges::find(LAUNCHERS, std::string_view(program)) == LAUNCHERS.end())
        {
            keys.push_back(program); // Not a launcher, which runs a script or an argument instead
        }
        break;
    }
    return keys;
}

/// Desktop entries' icons, by the keys a process is looked up by.
class DesktopIndex
{
  public:
    /// Adds @p entry, read from the desktop file with ID @p desktopId ("org.gnome.Nautilus"). Keys
    /// already taken keep their icon: entries are added in the directories' precedence order.
    void add(const DesktopEntry& entry, std::string_view desktopId)
    {
        if (entry.hidden || !entry.application || entry.icon.empty())
        {
            return;
        }
        for (const std::string& key : execProgramKeys(entry.exec))
        {
            m_IconByKey.try_emplace(key, entry.icon);
        }
        if (!entry.tryExec.empty())
        {
            m_IconByKey.try_emplace(foldCase(fileName(entry.tryExec)), entry.icon);
        }
        if (!entry.startupWmClass.empty())
        {
            m_IconByKey.try_emplace(foldCase(entry.startupWmClass), entry.icon);
        }
        const std::string id = foldCase(desktopId);
        m_IconByKey.try_emplace(id, entry.icon);
        // "org.mozilla.firefox" is also found as "firefox".
        if (const std::size_t dot = id.rfind('.'); dot != std::string::npos && dot + 1 < id.size())
        {
            m_IconByKey.try_emplace(id.substr(dot + 1), entry.icon);
        }
    }

    /// Reads every *.desktop file in @p applicationDirs, highest precedence first (subdirectories
    /// too: their IDs join the path with '-', as the spec says). Unreadable directories and files
    /// are skipped.
    void addDirectories(std::span<const std::filesystem::path> applicationDirs)
    {
        for (const std::filesystem::path& dir : applicationDirs)
        {
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            while (!ec && it != end)
            {
                const std::filesystem::directory_entry& file = *it;
                if (file.path().extension() == ".desktop" && file.is_regular_file(ec) && !ec)
                {
                    addFile(dir, file.path());
                }
                ec.clear();
                it.increment(ec);
            }
        }
    }

    /// The Icon value for a process running @p executablePath (may be empty) named @p processName.
    [[nodiscard]] std::optional<std::string> iconFor(std::string_view executablePath, std::string_view processName) const
    {
        const std::string byPath = foldCase(fileName(executablePath));
        if (!byPath.empty())
        {
            if (const auto it = m_IconByKey.find(byPath); it != m_IconByKey.end())
            {
                return it->second;
            }
        }
        const std::string byName = foldCase(processName);
        if (!byName.empty())
        {
            if (const auto it = m_IconByKey.find(byName); it != m_IconByKey.end())
            {
                return it->second;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_IconByKey.size();
    }

  private:
    void addFile(const std::filesystem::path& root, const std::filesystem::path& file)
    {
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(file, ec);
        if (ec || bytes > MAX_DESKTOP_FILE_BYTES)
        {
            return;
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return;
        }
        std::string text(static_cast<std::size_t>(bytes), '\0');
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(stream.gcount()));
        const std::optional<DesktopEntry> entry = parseDesktopEntry(text);
        if (!entry.has_value())
        {
            return;
        }
        // The desktop file ID: its path below the directory, '/' as '-', without ".desktop".
        std::string id = file.lexically_relative(root).replace_extension().generic_string();
        std::ranges::replace(id, '/', '-');
        add(*entry, id);
    }

    std::unordered_map<std::string, std::string> m_IconByKey;
};

/// THEME_SIZES in the order to try for @p sizePx: the sizes at or above it, nearest first, then the
/// ones below it, largest first -- a larger icon scales down cleanly, a smaller one blurs.
[[nodiscard]] inline std::vector<int> sizesToTry(int sizePx)
{
    std::vector<int> sizes;
    sizes.reserve(THEME_SIZES.size());
    for (const int size : THEME_SIZES)
    {
        if (size >= sizePx)
        {
            sizes.push_back(size);
        }
    }
    for (const int size : THEME_SIZES | std::views::reverse)
    {
        if (size < sizePx)
        {
            sizes.push_back(size);
        }
    }
    return sizes;
}

/// The PNG file for the desktop entry's Icon value @p icon near @p sizePx: an absolute path to a
/// PNG as is; a name looked up in the hicolor theme under each of @p iconBaseDirs (highest precedence
/// first) at the sizes sizesToTry() gives, then in @p pixmapDirs. nullopt when there is no PNG.
[[nodiscard]] inline std::optional<std::filesystem::path> findIconFile(std::string_view icon,
                                                                       int sizePx,
                                                                       std::span<const std::filesystem::path> iconBaseDirs,
                                                                       std::span<const std::filesystem::path> pixmapDirs)
{
    std::error_code ec;
    if (icon.empty())
    {
        return std::nullopt;
    }
    if (icon.front() == '/')
    {
        std::filesystem::path path{icon};
        if (path.extension() == ".png" && std::filesystem::is_regular_file(path, ec))
        {
            return path;
        }
        return std::nullopt; // An SVG or XPM by path: no decoder
    }
    if (icon.contains('/'))
    {
        return std::nullopt; // A relative path: not an icon name
    }
    // An Icon value may (wrongly, but commonly) carry the extension.
    std::string pngName(icon);
    if (!pngName.ends_with(".png"))
    {
        pngName += ".png";
    }
    for (const int size : sizesToTry(sizePx))
    {
        const std::string sizeDir = std::format("{}x{}", size, size);
        for (const std::filesystem::path& base : iconBaseDirs)
        {
            std::filesystem::path candidate = base / FALLBACK_THEME / sizeDir / "apps" / pngName;
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
    }
    for (const std::filesystem::path& dir : pixmapDirs)
    {
        std::filesystem::path candidate = dir / pngName;
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

/// Where desktop entries and icons are looked for.
struct SearchDirs
{
    std::vector<std::filesystem::path> applications; ///< Desktop entries, highest precedence first
    std::vector<std::filesystem::path> iconBases;    ///< Icon theme roots ($dir/icons), highest first
    std::vector<std::filesystem::path> pixmaps;      ///< Unthemed icons
};

/// The XDG directories, from $HOME, $XDG_DATA_HOME and $XDG_DATA_DIRS as given (empty when unset),
/// plus Flatpak's and Snap's exported entries: the user's data directory first, then the system's.
[[nodiscard]] inline SearchDirs searchDirs(std::string_view home, std::string_view xdgDataHome, std::string_view xdgDataDirs)
{
    std::vector<std::filesystem::path> dataDirs;
    if (!xdgDataHome.empty())
    {
        dataDirs.emplace_back(xdgDataHome);
    }
    else if (!home.empty())
    {
        dataDirs.push_back(std::filesystem::path(home) / ".local" / "share");
    }
    if (!home.empty())
    {
        dataDirs.push_back(std::filesystem::path(home) / ".local" / "share" / "flatpak" / "exports" / "share");
    }
    std::string_view systemDirs = xdgDataDirs.empty() ? std::string_view("/usr/local/share:/usr/share") : xdgDataDirs;
    while (!systemDirs.empty())
    {
        const std::size_t colon = systemDirs.find(':');
        const std::string_view dir = systemDirs.substr(0, colon);
        systemDirs.remove_prefix(colon == std::string_view::npos ? systemDirs.size() : colon + 1);
        if (!dir.empty())
        {
            dataDirs.emplace_back(dir);
        }
    }
    dataDirs.emplace_back("/var/lib/flatpak/exports/share");

    SearchDirs dirs;
    for (const std::filesystem::path& data : dataDirs)
    {
        if (std::ranges::find(dirs.applications, data / "applications") == dirs.applications.end())
        {
            dirs.applications.push_back(data / "applications");
            dirs.iconBases.push_back(data / "icons");
        }
    }
    dirs.applications.emplace_back("/var/lib/snapd/desktop/applications");
    if (!home.empty())
    {
        dirs.iconBases.insert(dirs.iconBases.begin(), std::filesystem::path(home) / ".icons");
    }
    dirs.pixmaps.emplace_back("/usr/share/pixmaps");
    return dirs;
}

} // namespace Platform::DesktopEntryIcons
