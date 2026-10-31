#pragma once

// Start TaskSmack when the user signs in (#3093, part 2): the decisions, free of the registry or any file.
// Platform::IStartAtLogin reads and writes the sign-in entry (on Windows the HKCU Run key's TaskSmack value);
// App::StartAtLoginSetting runs it on a worker. This says what that entry's text is, and what the entry it
// finds means for this copy: Off, On, or another copy of TaskSmack (a zip copy that moved, say), which
// Settings offers to repair. Header-only and pure, so it is compiled and tested on every host.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Domain::StartAtLogin
{

/// The argument the entry starts TaskSmack with: minimized, without taking the focus (main.cpp).
inline constexpr std::string_view MINIMIZED_ARGUMENT = "--minimized";

/// The entry's text for the TaskSmack at @p executable: its full path, quoted, so a path with spaces
/// ("C:\Program Files\TaskSmack\TaskSmack.exe") is one argument, then MINIMIZED_ARGUMENT.
[[nodiscard]] inline std::string runCommand(std::string_view executable)
{
    std::string command;
    command.reserve(executable.size() + MINIMIZED_ARGUMENT.size() + 3);
    command += '"';
    command += executable;
    command += "\" ";
    command += MINIMIZED_ARGUMENT;
    return command;
}

/// The program an entry's @p command starts, as Windows reads a command line: the quoted text when it
/// starts with a quote, else everything up to the first space. nullopt when there is none (an empty
/// command, or a quote that never closes).
[[nodiscard]] inline std::optional<std::string> commandProgram(std::string_view command)
{
    const std::size_t start = command.find_first_not_of(" \t");
    if (start == std::string_view::npos)
    {
        return std::nullopt;
    }
    command.remove_prefix(start);
    if (command.front() == '"')
    {
        const std::size_t close = command.find('"', 1);
        if (close == std::string_view::npos || close == 1)
        {
            return std::nullopt;
        }
        return std::string(command.substr(1, close - 1));
    }
    return std::string(command.substr(0, command.find_first_of(" \t")));
}

namespace Detail
{

/// @p path with '/' as '\', and ASCII letters lower-cased when @p ignoreCase, for comparing Windows paths.
[[nodiscard]] inline std::string comparable(std::string_view path, bool ignoreCase)
{
    std::string out(path);
    std::ranges::replace(out, '/', '\\');
    if (ignoreCase)
    {
        std::ranges::transform(out, out.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    }
    return out;
}

} // namespace Detail

/// Whether @p a and @p b name the same file: equal once '/' is read as '\' and, with @p ignoreCase (Windows),
/// ignoring the case of ASCII letters. An empty path is never the same as another.
[[nodiscard]] inline bool samePath(std::string_view a, std::string_view b, bool ignoreCase)
{
    return !a.empty() && !b.empty() && Detail::comparable(a, ignoreCase) == Detail::comparable(b, ignoreCase);
}

/// Whether @p path lies inside the directory @p directory (at any depth), compared as samePath() does.
[[nodiscard]] inline bool isInside(std::string_view path, std::string_view directory, bool ignoreCase)
{
    std::string dir = Detail::comparable(directory, ignoreCase);
    while (!dir.empty() && dir.back() == '\\')
    {
        dir.pop_back();
    }
    if (dir.empty())
    {
        return false;
    }
    dir += '\\';
    const std::string file = Detail::comparable(path, ignoreCase);
    return file.size() > dir.size() && file.starts_with(dir);
}

/// What the sign-in entry means for this copy.
enum class State : std::uint8_t
{
    Unknown,   ///< Not read yet
    Off,       ///< There is no entry: TaskSmack doesn't start at sign-in
    On,        ///< The entry starts this copy
    OtherCopy, ///< The entry starts something else: another copy, or text TaskSmack can't read. Repair rewrites it
    Failed,    ///< It couldn't be read, or changed
};

/// The sign-in entry, as Settings shows it.
struct Status
{
    std::string otherProgram; ///< OtherCopy: the program the entry starts; empty when it can't be read
    std::string error;        ///< Failed: why, for the line under the checkbox
    State state = State::Unknown;
    bool installed = false; ///< This copy is under Program Files; else Settings says to turn this off before moving it

    /// Whether the entry was read: Off, On or another copy's. Only then can the checkbox change it.
    [[nodiscard]] bool known() const noexcept
    {
        return state == State::Off || state == State::On || state == State::OtherCopy;
    }

    /// Whether the checkbox is ticked: there is an entry, this copy's or another's.
    [[nodiscard]] bool enabled() const noexcept
    {
        return state == State::On || state == State::OtherCopy;
    }

    friend bool operator==(const Status&, const Status&) = default;
};

/// The status for the entry's @p command (nullopt: there is none), read for the TaskSmack at @p executable.
/// @param installed   Whether @p executable is under Program Files (Status::installed).
/// @param ignoreCase  Whether paths compare ignoring case (Windows).
[[nodiscard]] inline Status
classify(const std::optional<std::string>& command, std::string_view executable, bool installed, bool ignoreCase)
{
    Status status;
    status.installed = installed;
    if (!command.has_value())
    {
        status.state = State::Off;
        return status;
    }
    std::optional<std::string> program = commandProgram(*command);
    if (program.has_value() && samePath(*program, executable, ignoreCase))
    {
        status.state = State::On;
        return status;
    }
    status.state = State::OtherCopy;
    status.otherProgram = std::move(program).value_or(std::string{});
    return status;
}

/// A status for a read or a change that failed, saying @p detail.
[[nodiscard]] inline Status failed(std::string detail)
{
    Status status;
    status.state = State::Failed;
    status.error = std::move(detail);
    return status;
}

/// The line Settings shows under the checkbox for @p status, or while a change (on, off, Repair) runs
/// (@p changing). A read again as Settings opens keeps the last status's line.
[[nodiscard]] inline std::string statusLine(const Status& status, bool changing)
{
    if (changing)
    {
        return "Updating\xE2\x80\xA6";
    }
    switch (status.state)
    {
    case State::Unknown:
        return "Checking\xE2\x80\xA6"; // the first read, as Settings opens
    case State::Off:
        return "Off: TaskSmack doesn't start when you sign in.";
    case State::On:
        return "On: this copy starts minimized when you sign in.";
    case State::OtherCopy:
        if (status.otherProgram.empty())
        {
            return "Points to a command TaskSmack can't read. Repair points it to this copy.";
        }
        return "Points to another copy: " + status.otherProgram + ". Repair points it to this copy.";
    case State::Failed:
        return "Couldn't read or change it: " + status.error;
    }
    return {};
}

/// The note for a copy outside Program Files (a zip or portable copy) while it starts at sign-in: the
/// entry would start a file that is gone once the copy is moved or deleted.
inline constexpr const char* PORTABLE_NOTE = "Turn this off before moving or deleting this copy.";

/// Whether to show PORTABLE_NOTE for @p status.
[[nodiscard]] inline bool showPortableNote(const Status& status) noexcept
{
    return status.state == State::On && !status.installed;
}

} // namespace Domain::StartAtLogin
