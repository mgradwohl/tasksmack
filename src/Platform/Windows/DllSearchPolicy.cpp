#include "DllSearchPolicy.h"

#include "Platform/IElevatedRestart.h"
#include "WindowsHandles.h"

#include <cstddef>
#include <string_view>

namespace Platform::Windows::DllSearchPolicy
{
namespace
{

/// Whether wide @p text is ASCII @p ascii, character for character.
[[nodiscard]] constexpr bool equalsAscii(std::wstring_view text, std::string_view ascii) noexcept
{
    if (text.size() != ascii.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != static_cast<wchar_t>(static_cast<unsigned char>(ascii[i])))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool isSeparator(wchar_t c) noexcept
{
    return c == L' ' || c == L'\t';
}

} // namespace

bool hasHandoffArgument(std::wstring_view commandLine) noexcept
{
    std::size_t position = 0;
    while (position < commandLine.size())
    {
        while (position < commandLine.size() && isSeparator(commandLine[position]))
        {
            ++position;
        }
        std::size_t end = position;
        while (end < commandLine.size() && !isSeparator(commandLine[end]))
        {
            ++end;
        }
        std::wstring_view token = commandLine.substr(position, end - position);
        while (token.starts_with(L'"'))
        {
            token.remove_prefix(1);
        }
        while (token.ends_with(L'"'))
        {
            token.remove_suffix(1);
        }
        if (equalsAscii(token, HANDOFF_ARGUMENT))
        {
            return true;
        }
        position = end;
    }
    return false;
}

bool system32Only(TOKEN_ELEVATION_TYPE elevation, std::wstring_view commandLine) noexcept
{
    return elevation == TokenElevationTypeFull || hasHandoffArgument(commandLine);
}

bool system32OnlyForThisProcess() noexcept
{
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    if (UniqueHandle token; OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) != FALSE)
    {
        DWORD size = 0;
        if (GetTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size) == FALSE)
        {
            type = TokenElevationTypeDefault;
        }
    }
    const wchar_t* const commandLine = GetCommandLineW();
    return system32Only(type, commandLine != nullptr ? std::wstring_view(commandLine) : std::wstring_view());
}

} // namespace Platform::Windows::DllSearchPolicy
