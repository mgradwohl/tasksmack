#pragma once

// XDG autostart entries (#2541): the .desktop files a desktop session starts at sign-in, from
// $XDG_CONFIG_HOME/autostart (~/.config/autostart) and each $XDG_CONFIG_DIRS/autostart (/etc/xdg/autostart),
// as the Desktop Application Autostart Specification reads them. A file in an earlier directory hides a
// file of the same name in a later one, which is how a user turns a system entry off without touching it:
// a copy in ~/.config/autostart with Hidden=true. Pure parsing and rewriting here; LinuxStartupProbe and
// LinuxStartupActions do the file system.

#include "Platform/AsciiTrim.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::XdgAutostart
{

/// What an autostart file's [Desktop Entry] group says (unlocalised keys only).
struct Entry
{
    std::string name;    ///< Name
    std::string exec;    ///< Exec: the command line, field codes (%f, %u, ...) and all
    std::string tryExec; ///< TryExec: the program whose absence means the entry is skipped
    std::string icon;    ///< Icon
    bool hidden = false; ///< Hidden=true: the entry is off (the spec's "deleted")
    /// X-GNOME-Autostart-enabled, when given: GNOME's and others' own off switch.
    std::optional<bool> gnomeEnabled;
    std::vector<std::string> onlyShowIn; ///< OnlyShowIn: the desktops it starts in, when given
    std::vector<std::string> notShowIn;  ///< NotShowIn: the desktops it doesn't start in
    bool application = true;             ///< Type=Application (or no Type)
    bool noDisplay = false;              ///< NoDisplay=true: not shown in menus or startup lists
    /// Started by the session itself, not a user's startup app: X-GNOME-Autostart-Phase given (accessibility,
    /// settings daemons, xdg-user-dirs), or X-GNOME-HiddenUnderSystemd=true (systemd starts it instead).
    bool sessionComponent = false;

    /// Whether the entry is switched on: not Hidden, and not X-GNOME-Autostart-enabled=false.
    [[nodiscard]] bool enabled() const noexcept
    {
        return !hidden && gnomeEnabled.value_or(true);
    }
};

/// The ';'-separated words of a desktop file's list value ("GNOME;KDE;"), empty ones dropped.
[[nodiscard]] inline std::vector<std::string> splitList(std::string_view value, char separator = ';')
{
    std::vector<std::string> words;
    while (!value.empty())
    {
        const std::size_t end = value.find(separator);
        const std::string_view word = trimAscii(value.substr(0, end));
        if (!word.empty())
        {
            words.emplace_back(word);
        }
        value.remove_prefix(end == std::string_view::npos ? value.size() : end + 1);
    }
    return words;
}

/// @p text without a leading UTF-8 byte order mark, which GLib's key file reader also skips.
[[nodiscard]] constexpr std::string_view withoutBom(std::string_view text) noexcept
{
    constexpr std::string_view BOM = "\xEF\xBB\xBF";
    if (text.starts_with(BOM))
    {
        text.remove_prefix(BOM.size());
    }
    return text;
}

/// The [Desktop Entry] group of an autostart file's @p text; nullopt when it has none.
[[nodiscard]] inline std::optional<Entry> parse(std::string_view text)
{
    text = withoutBom(text);
    Entry entry;
    bool inMainGroup = false;
    bool sawMainGroup = false;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view line = trimAscii(text.substr(0, newline));
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
        const std::size_t equals = line.find('=');
        if (!inMainGroup || equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = trimAscii(line.substr(0, equals));
        const std::string_view value = trimAscii(line.substr(equals + 1));
        if (key == "Name")
        {
            entry.name = value;
        }
        else if (key == "Exec")
        {
            entry.exec = value;
        }
        else if (key == "TryExec")
        {
            entry.tryExec = value;
        }
        else if (key == "Icon")
        {
            entry.icon = value;
        }
        else if (key == "Hidden")
        {
            entry.hidden = (value == "true");
        }
        else if (key == "X-GNOME-Autostart-enabled")
        {
            entry.gnomeEnabled = (value != "false");
        }
        else if (key == "OnlyShowIn")
        {
            entry.onlyShowIn = splitList(value);
        }
        else if (key == "NotShowIn")
        {
            entry.notShowIn = splitList(value);
        }
        else if (key == "Type")
        {
            entry.application = (value == "Application");
        }
        else if (key == "NoDisplay")
        {
            entry.noDisplay = (value == "true");
        }
        else if (key == "X-GNOME-Autostart-Phase")
        {
            entry.sessionComponent = entry.sessionComponent || !value.empty();
        }
        else if (key == "X-GNOME-HiddenUnderSystemd")
        {
            entry.sessionComponent = entry.sessionComponent || value == "true";
        }
    }
    if (!sawMainGroup)
    {
        return std::nullopt;
    }
    return entry;
}

/// Whether @p entry starts in the session whose $XDG_CURRENT_DESKTOP is @p currentDesktops (a
/// ':'-separated list, "ubuntu:GNOME"), as the Desktop Entry Specification says: the desktops in order, the
/// first one named in OnlyShowIn (starts) or NotShowIn (doesn't) decides; named in neither, it starts
/// unless it has an OnlyShowIn. With no desktop named, an OnlyShowIn entry doesn't start.
[[nodiscard]] inline bool startsIn(const Entry& entry, std::string_view currentDesktops)
{
    const auto listed = [](const std::vector<std::string>& list, const std::string& desktop)
    {
        return std::ranges::find(list, desktop) != list.end();
    };
    for (const std::string& desktop : splitList(currentDesktops, ':'))
    {
        if (listed(entry.onlyShowIn, desktop))
        {
            return true;
        }
        if (listed(entry.notShowIn, desktop))
        {
            return false;
        }
    }
    return entry.onlyShowIn.empty();
}

/// Where autostart files are looked for, the user's first: $XDG_CONFIG_HOME/autostart (else
/// ~/.config/autostart), then each $XDG_CONFIG_DIRS directory's (else /etc/xdg/autostart). Relative
/// directories are ignored, as the Base Directory Specification says.
struct Dirs
{
    std::filesystem::path user; ///< Empty when neither XDG_CONFIG_HOME nor HOME is usable.
    std::vector<std::filesystem::path> system;
};

[[nodiscard]] inline Dirs dirs(std::string_view home, std::string_view xdgConfigHome, std::string_view xdgConfigDirs)
{
    Dirs found;
    if (!xdgConfigHome.empty() && xdgConfigHome.front() == '/')
    {
        found.user = std::filesystem::path(xdgConfigHome) / "autostart";
    }
    else if (!home.empty() && home.front() == '/')
    {
        found.user = std::filesystem::path(home) / ".config" / "autostart";
    }
    for (const std::string& dir : splitList(xdgConfigDirs.empty() ? std::string_view("/etc/xdg") : xdgConfigDirs, ':'))
    {
        if (dir.front() == '/')
        {
            found.system.push_back(std::filesystem::path(dir) / "autostart");
        }
    }
    return found;
}

/// @p text with its [Desktop Entry] group switched on or off: Hidden set to @p enabled's opposite
/// (the line added right after the group header when there was none), and X-GNOME-Autostart-enabled,
/// when present, set to match. Every other line is kept as it was. nullopt when @p text has no
/// [Desktop Entry] group.
[[nodiscard]] inline std::optional<std::string> withEnabled(std::string_view text, bool enabled)
{
    // A byte order mark is kept, and the file's own line ending (CRLF or LF) is used for the lines written.
    const std::string_view bom = text.substr(0, text.size() - withoutBom(text).size());
    text = withoutBom(text);
    const std::string_view eol = text.contains("\r\n") ? std::string_view("\r\n") : std::string_view("\n");
    const std::string hiddenLine = std::string(enabled ? "Hidden=false" : "Hidden=true").append(eol);
    const std::string gnomeLine = std::string(enabled ? "X-GNOME-Autostart-enabled=true" : "X-GNOME-Autostart-enabled=false").append(eol);
    std::string out(bom);
    out.reserve(bom.size() + text.size() + hiddenLine.size() + eol.size());
    bool inMainGroup = false;
    bool sawMainGroup = false;
    bool wroteHidden = false;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        // Each line with its own ending, so the file's are kept as they were.
        const std::string_view rawLine = text.substr(0, newline == std::string_view::npos ? text.size() : newline + 1);
        text.remove_prefix(rawLine.size());
        const std::string_view line = trimAscii(rawLine);
        const std::string_view ending = rawLine.ends_with('\n') ? std::string_view{} : eol; // the last line gains one
        if (!line.empty() && line.front() == '[')
        {
            inMainGroup = (line == "[Desktop Entry]");
            out.append(rawLine).append(ending);
            if (inMainGroup && !sawMainGroup)
            {
                sawMainGroup = true;
                out.append(hiddenLine);
                wroteHidden = true;
            }
            continue;
        }
        if (inMainGroup)
        {
            const std::size_t equals = line.find('=');
            const std::string_view key = equals == std::string_view::npos ? std::string_view{} : trimAscii(line.substr(0, equals));
            if (key == "Hidden")
            {
                continue; // written after the header
            }
            if (key == "X-GNOME-Autostart-enabled")
            {
                out.append(gnomeLine);
                continue;
            }
        }
        out.append(rawLine).append(ending);
    }
    if (!wroteHidden)
    {
        return std::nullopt;
    }
    return out;
}

} // namespace Platform::XdgAutostart
