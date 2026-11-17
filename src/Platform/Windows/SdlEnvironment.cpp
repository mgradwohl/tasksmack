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

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows::SdlEnvironment
{
namespace
{

/// @p c in upper case, for ASCII letters; anything else as it is.
[[nodiscard]] constexpr wchar_t asciiUpper(wchar_t c) noexcept
{
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

[[nodiscard]] constexpr bool isAsciiDigit(wchar_t c) noexcept
{
    return c >= L'0' && c <= L'9';
}

/// What removeForThisProcess() removes if it can't read the environment block: the variables that name a library SDL
/// loads (SDL 3.4's *_LIBRARY hints, and SDL3_DYNAMIC_API) or pick the video driver.
constexpr std::array<std::wstring_view, 8> FALLBACK_NAMES = {
    L"SDL3_DYNAMIC_API",
    L"SDL_OPENGL_LIBRARY",
    L"SDL_EGL_LIBRARY",
    L"SDL_VULKAN_LIBRARY",
    L"SDL_OPENVR_LIBRARY",
    L"SDL_X11_XCB_LIBRARY",
    L"SDL_VIDEO_DRIVER",
    L"SDL_VIDEODRIVER",
};

/// The names removeForThisProcess() last removed, in UTF-8.
std::vector<std::string>& removedNames()
{
    static std::vector<std::string> names;
    return names;
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

std::vector<std::wstring>
removeSdlVariables(bool system32Only, std::wstring_view block, const std::function<bool(const std::wstring&)>& remove)
{
    std::vector<std::wstring> removed;
    if (!system32Only)
    {
        return removed;
    }
    for (std::wstring& name : sdlVariableNames(block))
    {
        if (remove(name))
        {
            removed.push_back(std::move(name));
        }
    }
    return removed;
}

std::vector<std::wstring> removeForThisProcess(bool system32Only)
{
    std::vector<std::wstring> removed;
    if (system32Only)
    {
        // A copy of the block: removing a variable while walking the live one would change what is walked.
        std::wstring block;
        if (wchar_t* const strings = GetEnvironmentStringsW(); strings != nullptr)
        {
            std::size_t length = 0;
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the block ends at its first empty string
            while (strings[length] != L'\0' || strings[length + 1] != L'\0')
            {
                ++length;
            }
            block.assign(strings, length + 1);
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            FreeEnvironmentStringsW(strings);
        }
        else
        {
            // The block couldn't be read: at least the variables that name a library SDL loads, or pick a video driver.
            for (const std::wstring_view name : FALLBACK_NAMES)
            {
                block.append(name).append(L"=").push_back(L'\0');
            }
        }
        removed = removeSdlVariables(
            true, block, [](const std::wstring& name) { return SetEnvironmentVariableW(name.c_str(), nullptr) != FALSE; });
    }
    std::vector<std::string>& names = removedNames();
    names.clear();
    for (const std::wstring& name : removed)
    {
        names.push_back(Platform::WinString::wideToUtf8(name));
    }
    return removed;
}

const std::vector<std::string>& removedFromThisProcess() noexcept
{
    return removedNames();
}

} // namespace Platform::Windows::SdlEnvironment
