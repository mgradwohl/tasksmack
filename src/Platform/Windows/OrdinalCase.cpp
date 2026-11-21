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

} // namespace Platform::Windows
