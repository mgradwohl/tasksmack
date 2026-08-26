#include "WinString.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include <windows.h>

namespace Platform::WinString
{

[[nodiscard]] std::string wideToUtf8(const wchar_t* wide)
{
    if (wide == nullptr || wide[0] == L'\0')
    {
        return {};
    }

    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
    {
        return {};
    }

    // sizeNeeded includes null terminator; allocate string length only
    std::string result(static_cast<size_t>(sizeNeeded - 1), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), sizeNeeded, nullptr, nullptr);
    if (written <= 0)
    {
        return {};
    }
    return result;
}

[[nodiscard]] std::string wideToUtf8(const std::wstring& wide)
{
    return wideToUtf8(wide.c_str());
}

[[nodiscard]] std::string wideToUtf8(std::wstring_view wide)
{
    if (wide.empty())
    {
        return {};
    }

    // WideCharToMultiByte requires an int length; reject views that cannot be represented.
    // OS-provided strings never approach INT_MAX characters, so this is effectively unreachable.
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }
    // Safe and necessary: bounds-checked above.
    const int length = static_cast<int>(wide.size());
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - explicit length is passed alongside data()
    const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(sizeNeeded), '\0');
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - explicit length is passed alongside data()
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, result.data(), sizeNeeded, nullptr, nullptr);
    if (written <= 0)
    {
        return {};
    }
    result.resize(static_cast<size_t>(written));
    return result;
}

[[nodiscard]] std::wstring utf8ToWide(std::string_view utf8)
{
    if (utf8.empty())
    {
        return {};
    }

    const int sizeNeeded = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (sizeNeeded <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(sizeNeeded), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), sizeNeeded);
    if (written <= 0)
    {
        return {};
    }
    result.resize(static_cast<size_t>(written));
    return result;
}

} // namespace Platform::WinString
