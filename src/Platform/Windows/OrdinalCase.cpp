#include "OrdinalCase.h"

#include "WinString.h"

#include <climits>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Platform::Windows
{

namespace
{

[[nodiscard]] constexpr bool isSeparator(wchar_t unit) noexcept
{
    return unit == L'\\' || unit == L'/';
}

/// Whether @p text, a path without its trailing separators, is a drive: "C:", or the device paths "\\?\C:" and "\\.\C:".
[[nodiscard]] constexpr bool isDrive(std::wstring_view text) noexcept
{
    constexpr std::size_t DEVICE_PREFIX = 4; // \\?\ or \\.\, either separator
    if (text.size() == DEVICE_PREFIX + 2 && isSeparator(text[0]) && isSeparator(text[1]) && (text[2] == L'?' || text[2] == L'.') &&
        isSeparator(text[3]))
    {
        text.remove_prefix(DEVICE_PREFIX);
    }
    return text.size() == 2 && text[1] == L':' && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'));
}

/// A path as samePathIgnoringTrailingSeparators() compares it: its text without trailing separators, and whether one
/// was dropped from a root (a drive, or separators alone), where it counts.
struct ComparedPath
{
    std::wstring_view text;
    bool root = false;
};

[[nodiscard]] constexpr ComparedPath comparedPath(std::wstring_view path) noexcept
{
    const std::size_t length = path.size();
    while (!path.empty() && isSeparator(path.back()))
    {
        path.remove_suffix(1);
    }
    return {.text = path, .root = path.size() != length && (path.empty() || isDrive(path))};
}

} // namespace

bool equalsOrdinalIgnoreCase(const std::string_view a, const std::string_view b) noexcept
{
    try
    {
        const std::wstring wideA = WinString::utf8ToWide(a);
        const std::wstring wideB = WinString::utf8ToWide(b);
        if ((wideA.empty() && !a.empty()) || (wideB.empty() && !b.empty()) || wideA.size() > static_cast<std::size_t>(INT_MAX) ||
            wideB.size() > static_cast<std::size_t>(INT_MAX))
        {
            return a == b;
        }
        return equalsOrdinalIgnoreCase(std::wstring_view(wideA), std::wstring_view(wideB));
    }
    catch (const std::bad_alloc&)
    {
        return a == b;
    }
}

bool equalsOrdinalIgnoreCase(const std::wstring_view a, const std::wstring_view b) noexcept
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool samePathIgnoringTrailingSeparators(const std::wstring_view a, const std::wstring_view b) noexcept
{
    const ComparedPath first = comparedPath(a);
    const ComparedPath second = comparedPath(b);
    return first.root == second.root && equalsOrdinalIgnoreCase(first.text, second.text);
}

} // namespace Platform::Windows
