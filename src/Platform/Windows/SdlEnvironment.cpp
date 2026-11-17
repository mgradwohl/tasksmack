#include "SdlEnvironment.h"

#include "WinString.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows::SdlEnvironment
{
namespace
{

/// How many times the by-name pass removes one name that is still set: more than one entry can answer to a name.
constexpr int MAX_REMOVALS_PER_NAME = 8;

/// @p c in upper case, for ASCII letters; anything else as it is.
[[nodiscard]] constexpr wchar_t asciiUpper(wchar_t c) noexcept
{
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

[[nodiscard]] constexpr bool isAsciiDigit(wchar_t c) noexcept
{
    return c >= L'0' && c <= L'9';
}

[[nodiscard]] bool equalsIgnoringAsciiCase(std::wstring_view a, std::wstring_view b) noexcept
{
    return std::ranges::equal(a, b, [](wchar_t x, wchar_t y) { return asciiUpper(x) == asciiUpper(y); });
}

[[nodiscard]] constexpr bool isHighSurrogate(wchar_t c) noexcept
{
    return c >= 0xD800 && c <= 0xDBFF;
}

/// The names removeForThisProcess() last removed, as loggedNames() gives them.
std::vector<std::string>& removedNames()
{
    static std::vector<std::string> names;
    return names;
}

/// The names removeForThisProcess() last found still set after trying to remove them.
std::vector<std::wstring>& stillSetNames()
{
    static std::vector<std::wstring> names;
    return names;
}

/// Adds @p name to @p names unless one matching it, ignoring ASCII case, is there already.
void addOnce(std::vector<std::wstring>& names, const std::wstring& name)
{
    if (std::ranges::none_of(names, [&name](const std::wstring& already) { return equalsIgnoringAsciiCase(already, name); }))
    {
        names.push_back(name);
    }
}

/// Bidirectional controls, which can reorder the text around them in a log viewer: the marks U+200E and U+200F, the
/// embeddings and overrides U+202A to U+202E, and the isolates U+2066 to U+2069.
[[nodiscard]] constexpr bool isBidiControl(unsigned code) noexcept
{
    return code == 0x200E || code == 0x200F || (code >= 0x202A && code <= 0x202E) || (code >= 0x2066 && code <= 0x2069);
}

} // namespace

bool isSdlVariable(std::wstring_view name) noexcept
{
    if (name.size() < 4 || asciiUpper(name[0]) != L'S' || asciiUpper(name[1]) != L'D' || asciiUpper(name[2]) != L'L')
    {
        return false;
    }
    std::size_t position = 3;
    while (position < name.size() && isAsciiDigit(name[position]))
    {
        ++position;
    }
    return position < name.size() && name[position] == L'_';
}

std::vector<std::wstring> sdlVariableNames(std::wstring_view block)
{
    std::vector<std::wstring> names;
    std::size_t position = 0;
    while (position < block.size() && block[position] != L'\0')
    {
        std::size_t end = block.find(L'\0', position);
        if (end == std::wstring_view::npos)
        {
            end = block.size();
        }
        const std::wstring_view entry = block.substr(position, end - position);
        // A name ends at its first '='. The per-drive entries ("=C:=C:\dir") start with one, and aren't variables.
        if (const std::size_t equals = entry.find(L'='); equals != std::wstring_view::npos && equals > 0)
        {
            if (const std::wstring_view name = entry.substr(0, equals); isSdlVariable(name))
            {
                names.emplace_back(name);
            }
        }
        position = end + 1;
    }
    return names;
}

std::vector<std::wstring> removeSdlVariables(bool system32Only,
                                             std::optional<std::wstring_view> block,
                                             const EnvironmentOps& environment,
                                             std::vector<std::wstring>* stillSet)
{
    std::vector<std::wstring> removed;
    if (!system32Only)
    {
        return removed;
    }
    if (block.has_value())
    {
        for (std::wstring& name : sdlVariableNames(*block))
        {
            if (environment.remove(name))
            {
                removed.push_back(std::move(name));
            }
            else if (stillSet != nullptr && environment.isSet(name))
            {
                addOnce(*stillSet, name);
            }
        }
    }
    // By name too, whether or not the block was read: the OS matches a name its own way, so this finds a variant of a
    // dangerous name the block's ASCII fold missed, and reports a name only if one was set before it was removed.
    for (const std::wstring_view nameView : BY_NAME_VARIABLES)
    {
        const std::wstring name(nameView);
        bool removedOne = false;
        for (int pass = 0; pass < MAX_REMOVALS_PER_NAME && environment.isSet(name); ++pass)
        {
            if (!environment.remove(name))
            {
                break;
            }
            removedOne = true;
        }
        if (removedOne &&
            std::ranges::none_of(removed, [&name](const std::wstring& already) { return equalsIgnoringAsciiCase(already, name); }))
        {
            removed.push_back(name);
        }
        // Still set after the passes ran out, or the removal failed: nothing here can make the process safe to run.
        if (stillSet != nullptr && environment.isSet(name))
        {
            addOnce(*stillSet, name);
        }
    }
    return removed;
}

std::string logName(std::wstring_view name)
{
    const bool cut = name.size() > MAX_LOGGED_NAME;
    std::wstring_view kept = name.substr(0, std::min(name.size(), MAX_LOGGED_NAME));
    if (cut && !kept.empty() && isHighSurrogate(kept.back()))
    {
        kept.remove_suffix(1); // Not half a character
    }
    std::wstring safe;
    safe.reserve(kept.size());
    for (const wchar_t c : kept)
    {
        const auto code = static_cast<unsigned>(c);
        if (c == L'\\')
        {
            safe += L"\\\\"; // So an escape written below can't be forged with a literal one
        }
        else if (code < 0x20 || (code >= 0x7F && code <= 0x9F))
        {
            safe += std::format(L"\\x{:02X}", code);
        }
        else if (code == 0x2028 || code == 0x2029 || isBidiControl(code))
        {
            safe += std::format(L"\\u{:04X}", code);
        }
        else
        {
            safe.push_back(c);
        }
    }
    if (cut)
    {
        safe += L"...";
    }
    return Platform::WinString::wideToUtf8(safe);
}

std::vector<std::string> loggedNames(const std::vector<std::wstring>& names)
{
    std::vector<std::string> logged;
    const std::size_t shown = std::min(names.size(), MAX_LOGGED_NAMES);
    logged.reserve(shown + 1);
    for (std::size_t index = 0; index < shown; ++index)
    {
        logged.push_back(logName(names[index]));
    }
    if (names.size() > shown)
    {
        logged.push_back(std::format("and {} more", names.size() - shown));
    }
    return logged;
}

std::vector<std::wstring> removeForThisProcess(bool system32Only)
{
    stillSetNames().clear();
    std::vector<std::wstring> removed;
    if (system32Only)
    {
        // A copy of the block: removing a variable while walking the live one would change what is walked.
        std::optional<std::wstring> block;
        if (wchar_t* const strings = GetEnvironmentStringsW(); strings != nullptr)
        {
            std::size_t length = 0;
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the block ends at its first empty string
            while (strings[length] != L'\0' || strings[length + 1] != L'\0')
            {
                ++length;
            }
            block.emplace(strings, length + 1);
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            FreeEnvironmentStringsW(strings);
        }
        const EnvironmentOps environment{
            .isSet = [](const std::wstring& name) { return GetEnvironmentVariableW(name.c_str(), nullptr, 0) > 0; },
            .remove =
                [](const std::wstring& name)
            {
                // The CRT's copy first, best effort (it also clears the block's); then the block, which is what counts.
                static_cast<void>(_wputenv_s(name.c_str(), L""));
                return SetEnvironmentVariableW(name.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ENVVAR_NOT_FOUND;
            },
        };
        removed = removeSdlVariables(
            true, block.has_value() ? std::optional<std::wstring_view>(*block) : std::nullopt, environment, &stillSetNames());
    }
    removedNames() = loggedNames(removed);
    return removed;
}

const std::vector<std::string>& removedFromThisProcess() noexcept
{
    return removedNames();
}

const std::vector<std::wstring>& stillSetInThisProcess() noexcept
{
    return stillSetNames();
}

} // namespace Platform::Windows::SdlEnvironment
