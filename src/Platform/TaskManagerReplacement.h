#pragma once

// TaskSmack in place of Task Manager (#3072): Windows' Image File Execution Options "Debugger" value on
// taskmgr.exe, which Process Explorer and System Informer use too. With it set, Windows starts
//   <Debugger> C:\Windows\System32\taskmgr.exe [Task Manager's arguments]
// whenever anything starts Task Manager: Ctrl+Shift+Esc, the taskbar's menu, Ctrl+Alt+Del's screen.
//
// The value lives under HKLM, so it is written by an administrator only: TaskSmackHelper.exe, or an
// elevated TaskSmack itself. Both go through the logic here, which is pure and platform-neutral so it is
// tested on every platform with a fake registry (tests/Platform/test_TaskManagerReplacement.cpp):
//   - a path is checked before it is written (validateTaskSmackPath()): an absolute drive path to a file
//     named TaskSmack.exe, nothing else, so the helper can't be used to set an arbitrary debugger;
//   - a Debugger another program set (Process Explorer's) is never replaced unless the request says the
//     user confirmed it, and never removed;
//   - turning it off removes the Debugger value only, and the taskmgr.exe key too only when TaskSmack
//     created it (its CREATED_MARKER_VALUE says so) and nothing else is left in it.
// The uninstaller does the same removal in NSIS (cmake/CPackProjectConfig.cmake.in).

#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"
#include "Platform/IProcessActions.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Platform::TaskManagerReplacement
{

/// The key, under HKEY_LOCAL_MACHINE, in its 64-bit view.
inline constexpr std::string_view IFEO_KEY = R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe)";
/// The value Windows reads: the command line it starts instead of Task Manager.
inline constexpr std::string_view DEBUGGER_VALUE = "Debugger";
/// TaskSmack's own value in the key, set only when TaskSmack created the key: the key may then be deleted
/// once it is empty again. A key that was there before (another tool's) is never deleted.
inline constexpr std::string_view CREATED_MARKER_VALUE = "TaskSmackCreatedKey";
/// The only file name a Debugger TaskSmack writes may name.
inline constexpr std::string_view TASKSMACK_FILE_NAME = "TaskSmack.exe";
/// The longest path accepted, in UTF-8 bytes: Windows' longest path is 32767 UTF-16 units.
inline constexpr std::size_t MAX_PATH_BYTES = 32767;

/// What the Debugger value says.
enum class State : std::uint8_t
{
    Off,            ///< No Debugger value (or an empty one): Task Manager opens
    ThisCopy,       ///< It starts the TaskSmack that is running
    OtherTaskSmack, ///< It starts another TaskSmack.exe: a copy that moved, was deleted, or is installed elsewhere
    OtherProgram,   ///< It starts another program, such as Process Explorer
};

/// A Debugger value read and sorted.
struct Status
{
    State state = State::Off;
    /// The program the value starts, when it isn't Off (the value's first word, without quotes).
    std::string program;
};

/// The file name in @p path: what follows its last backslash or slash.
[[nodiscard]] constexpr std::string_view fileNameOf(std::string_view path) noexcept
{
    const std::size_t slash = path.find_last_of("\\/");
    if (slash == std::string_view::npos)
    {
        return path;
    }
    path.remove_prefix(slash + 1);
    return path;
}

/// Whether @p program is a TaskSmack.exe, wherever it is (ASCII letters ignoring case, as NTFS does).
[[nodiscard]] constexpr bool isTaskSmackProgram(std::string_view program) noexcept
{
    return equalsIgnoreCase(fileNameOf(program), TASKSMACK_FILE_NAME);
}

/// The program a Debugger value starts: the quoted first word without its quotes, or, unquoted, up to and
/// including the first ".exe" that ends a word (Windows' own reading of an unquoted path with spaces
/// tries each space in turn; one that names an .exe is what a tool writes), or else up to the first
/// space. Empty for an empty or all-blank value.
[[nodiscard]] constexpr std::string_view programOf(std::string_view debugger) noexcept
{
    debugger = trimAscii(debugger);
    if (debugger.empty())
    {
        return {};
    }
    if (debugger.front() == '"')
    {
        debugger.remove_prefix(1);
        const std::size_t close = debugger.find('"');
        if (close != std::string_view::npos)
        {
            debugger.remove_suffix(debugger.size() - close);
        }
        return debugger;
    }
    constexpr std::string_view EXE = ".exe";
    for (std::size_t at = 0; at + EXE.size() <= debugger.size(); ++at)
    {
        const std::size_t end = at + EXE.size();
        if (equalsIgnoreCase(std::string_view(debugger.data() + at, EXE.size()), EXE) &&
            (end == debugger.size() || debugger[end] == ' ' || debugger[end] == '\t'))
        {
            debugger.remove_suffix(debugger.size() - end);
            return debugger;
        }
    }
    const std::size_t space = debugger.find_first_of(" \t");
    if (space != std::string_view::npos)
    {
        debugger.remove_suffix(debugger.size() - space);
    }
    return debugger;
}

/// Whether @p a and @p b are the same path, ASCII letters ignoring case. A path that differs only in a
/// non-ASCII letter's case reads as another copy, which only offers a repair that isn't needed.
[[nodiscard]] constexpr bool samePath(std::string_view a, std::string_view b) noexcept
{
    return equalsIgnoreCase(a, b);
}

/// @p debugger (nullopt: no value) sorted against @p selfPath, the running TaskSmack's own image.
[[nodiscard]] inline Status classify(const std::optional<std::string>& debugger, std::string_view selfPath)
{
    if (!debugger.has_value())
    {
        return {};
    }
    const std::string_view program = programOf(*debugger);
    if (program.empty())
    {
        return {};
    }
    Status status{.state = State::OtherProgram, .program = std::string(program)};
    if (isTaskSmackProgram(program))
    {
        status.state = (!selfPath.empty() && samePath(program, selfPath)) ? State::ThisCopy : State::OtherTaskSmack;
    }
    return status;
}

/// Whether @p c is a character no path TaskSmack writes may hold: a control character, a quote (which
/// would end the quoted Debugger early and start an argument), or one of Windows' reserved characters.
/// '%' too: the value is REG_SZ, never expanded, but a path with one is no path TaskSmack runs from.
[[nodiscard]] constexpr bool isForbiddenPathChar(char c) noexcept
{
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F)
    {
        return true;
    }
    constexpr std::string_view FORBIDDEN = "\"<>|?*%/";
    return FORBIDDEN.find(c) != std::string_view::npos;
}

/// Why @p path can't be a Debugger TaskSmack writes, or nothing when it can: an absolute path on a drive
/// letter ("C:\..."), never a UNC path ("\\server\share"), a device or extended path ("\\.\", "\\?\") or a
/// relative one; backslashes only; no empty, "." or ".." folder, none ending in a space or a dot (which
/// Windows drops, so two spellings would name one file); no colon past the drive's (an alternate data
/// stream); no quote or reserved character; and the file named exactly TaskSmack.exe (letters ignoring
/// case). The helper then also checks that it is the TaskSmack.exe beside it (BrokerHelper::sameFile()).
[[nodiscard]] constexpr std::expected<void, std::string_view> validateTaskSmackPath(std::string_view path) noexcept
{
    if (path.empty())
    {
        return std::unexpected(std::string_view("the path is empty"));
    }
    if (path.size() > MAX_PATH_BYTES)
    {
        return std::unexpected(std::string_view("the path is too long"));
    }
    for (const char c : path)
    {
        if (isForbiddenPathChar(c))
        {
            return std::unexpected(std::string_view("the path holds a character no TaskSmack path has"));
        }
    }
    const bool driveLetter =
        path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' && path[2] == '\\';
    if (!driveLetter)
    {
        return std::unexpected(std::string_view("the path isn't an absolute path on a drive"));
    }
    std::string_view rest = path;
    rest.remove_prefix(3);
    if (rest.find(':') != std::string_view::npos)
    {
        return std::unexpected(std::string_view("the path names a stream, not a file"));
    }
    while (true)
    {
        const std::size_t slash = rest.find('\\');
        std::string_view part = rest;
        if (slash != std::string_view::npos)
        {
            part.remove_suffix(part.size() - slash);
        }
        if (part.empty() || part == "." || part == ".." || part.back() == ' ' || part.back() == '.' || part.front() == ' ')
        {
            return std::unexpected(std::string_view("the path has an empty, relative or trailing-dot folder"));
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    if (!isTaskSmackProgram(path))
    {
        return std::unexpected(std::string_view("the path isn't a TaskSmack.exe"));
    }
    return {};
}

/// The Debugger value for @p path: quoted, so a path with spaces is one word and Task Manager's own
/// command line follows as arguments.
[[nodiscard]] inline std::string debuggerFor(std::string_view path)
{
    std::string value;
    value.reserve(path.size() + 2);
    value += '"';
    value += path;
    value += '"';
    return value;
}

/// Whether @p selfPath is under one of @p installFolders (Program Files, in both views): an installed
/// copy, which the uninstaller cleans up after. Anything else (a per-user folder, an unpacked .zip) is a
/// copy the user may delete by hand, and Settings warns them to turn this off first.
[[nodiscard]] constexpr bool isInstalledCopy(std::string_view selfPath, std::span<const std::string_view> installFolders) noexcept
{
    for (std::string_view folder : installFolders)
    {
        while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/'))
        {
            folder.remove_suffix(1);
        }
        if (folder.empty() || selfPath.size() <= folder.size() + 1)
        {
            continue;
        }
        if (equalsIgnoreCase(std::string_view(selfPath.data(), folder.size()), folder) && selfPath[folder.size()] == '\\')
        {
            return true;
        }
    }
    return false;
}

/// Whether the startup check offers to repair or turn off the replacement (#3072): it starts another
/// TaskSmack.exe, and that one is gone (it moved, or was deleted) or this copy is the installed one (so
/// it is the one that should answer). Another TaskSmack that still exists while this copy is a portable
/// one isn't asked about: an installed TaskSmack owns it, and this copy doesn't nag every start.
[[nodiscard]] constexpr bool offersRepair(State state, bool otherProgramExists, bool installedCopy) noexcept
{
    return state == State::OtherTaskSmack && (!otherProgramExists || installedCopy);
}

/// The registry the replacement is written to: HKLM's taskmgr.exe key on Windows
/// (WindowsIfeoRegistry), or a test's fake. Every call is one registry operation; an error says why it
/// failed, in words for the user.
class IIfeoRegistry
{
  public:
    virtual ~IIfeoRegistry() = default;
    IIfeoRegistry() = default;
    IIfeoRegistry(const IIfeoRegistry&) = delete;
    IIfeoRegistry& operator=(const IIfeoRegistry&) = delete;
    IIfeoRegistry(IIfeoRegistry&&) = delete;
    IIfeoRegistry& operator=(IIfeoRegistry&&) = delete;

    /// The Debugger value; nullopt when the key or the value isn't there.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, std::string> readDebugger() = 0;
    /// Opens the key, creating it if it isn't there: true when it was created now.
    [[nodiscard]] virtual std::expected<bool, std::string> createKey() = 0;
    [[nodiscard]] virtual std::expected<void, std::string> writeDebugger(std::string_view value) = 0;
    /// Removes the Debugger value; one that isn't there is no error.
    [[nodiscard]] virtual std::expected<void, std::string> deleteDebugger() = 0;
    /// Whether CREATED_MARKER_VALUE is in the key.
    [[nodiscard]] virtual std::expected<bool, std::string> readCreatedMarker() = 0;
    [[nodiscard]] virtual std::expected<void, std::string> writeCreatedMarker() = 0;
    /// Removes CREATED_MARKER_VALUE; one that isn't there is no error.
    [[nodiscard]] virtual std::expected<void, std::string> deleteCreatedMarker() = 0;
    /// Deletes the key when it holds no value and no subkey: true when it was deleted.
    [[nodiscard]] virtual std::expected<bool, std::string> deleteKeyIfEmpty() = 0;
};

/// The words for a Debugger another program set, which a change leaves alone.
[[nodiscard]] inline std::string otherProgramMessage(std::string_view program)
{
    return "Task Manager is already replaced by " + std::string(program) + ". TaskSmack changes it only when you confirm.";
}

/// Points Task Manager at @p path (checked again here with validateTaskSmackPath()) in @p registry. A
/// Debugger of another program is replaced only with @p replaceOther (the user confirmed it); one of a
/// TaskSmack (this copy or another) is replaced as it is. A key this creates is marked as TaskSmack's.
[[nodiscard]] inline ProcessActionResult applySet(IIfeoRegistry& registry, std::string_view path, bool replaceOther)
{
    if (const auto valid = validateTaskSmackPath(path); !valid.has_value())
    {
        return ProcessActionResult::error("TaskSmack won't replace Task Manager with that program: " + std::string(valid.error()));
    }
    const auto current = registry.readDebugger();
    if (!current.has_value())
    {
        return ProcessActionResult::error("Couldn't read Task Manager's setting: " + current.error());
    }
    const Status status = classify(*current, path);
    if (status.state == State::OtherProgram && !replaceOther)
    {
        return ProcessActionResult::error(otherProgramMessage(status.program));
    }
    const auto created = registry.createKey();
    if (!created.has_value())
    {
        return ProcessActionResult::error("Couldn't open Task Manager's setting: " + created.error());
    }
    if (*created)
    {
        // Best effort: without the mark the key is only left behind, empty, when this is turned off.
        static_cast<void>(registry.writeCreatedMarker());
    }
    if (const auto written = registry.writeDebugger(debuggerFor(path)); !written.has_value())
    {
        return ProcessActionResult::error("Couldn't replace Task Manager: " + written.error());
    }
    return ProcessActionResult::ok("TaskSmack now opens in place of Task Manager");
}

/// Removes the key's CREATED_MARKER_VALUE and then the key, when TaskSmack created it and it is empty.
inline void removeKeyIfTaskSmacks(IIfeoRegistry& registry)
{
    const auto marked = registry.readCreatedMarker();
    if (!marked.has_value() || !*marked)
    {
        return; // another tool's key, or none: left as it is
    }
    if (registry.deleteCreatedMarker().has_value())
    {
        static_cast<void>(registry.deleteKeyIfEmpty());
    }
}

/// Gives Task Manager back: removes a Debugger that starts a TaskSmack.exe (this copy or another), never
/// another program's, then the key if TaskSmack created it and nothing else is left in it.
[[nodiscard]] inline ProcessActionResult applyClear(IIfeoRegistry& registry)
{
    const auto current = registry.readDebugger();
    if (!current.has_value())
    {
        return ProcessActionResult::error("Couldn't read Task Manager's setting: " + current.error());
    }
    const Status status = classify(*current, {});
    if (status.state == State::OtherProgram)
    {
        return ProcessActionResult::error("Task Manager is replaced by " + status.program + ", not by TaskSmack, so TaskSmack leaves it");
    }
    if (status.state != State::Off)
    {
        if (const auto deleted = registry.deleteDebugger(); !deleted.has_value())
        {
            return ProcessActionResult::error("Couldn't give Task Manager back: " + deleted.error());
        }
    }
    removeKeyIfTaskSmacks(registry);
    return ProcessActionResult::ok("Task Manager opens again");
}

} // namespace Platform::TaskManagerReplacement
