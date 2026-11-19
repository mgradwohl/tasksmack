#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Platform::WinString
{

/// Up to the first NUL. A lone surrogate becomes U+FFFD, as in every overload.
[[nodiscard]] std::string wideToUtf8(const wchar_t* wide);
/// All of @p wide, size() units, embedded NULs included (#4307).
[[nodiscard]] std::string wideToUtf8(const std::wstring& wide);
[[nodiscard]] std::string wideToUtf8(std::wstring_view wide);
/// All of @p wide as text to show (#4307): as wideToUtf8, with each NUL shown as U+2400 (a visible symbol), so a
/// name holding one ("OneDrive\0x") neither looks like another name nor shows blank.
[[nodiscard]] std::string wideToDisplayUtf8(std::wstring_view wide);
[[nodiscard]] std::wstring utf8ToWide(std::string_view utf8);

/// The system's words for Win32 error @p error, as UTF-8 (#4308): FormatMessageW, never the ANSI code page's
/// FormatMessageA that the standard library's system_category message calls, with the trailing period, spaces and line break
/// trimmed. "error N" when the system has no text for it. @p error is a DWORD.
[[nodiscard]] std::string win32ErrorText(unsigned long error);

/// Whether @p wide holds an unpaired surrogate (#4310): then wideToUtf8() shows it as U+FFFD, and that text no longer
/// names what @p wide names, so it must not be turned back into a path, a command or a registry value.
[[nodiscard]] constexpr bool hasLoneSurrogate(std::wstring_view wide) noexcept
{
    for (std::size_t i = 0; i < wide.size(); ++i)
    {
        const wchar_t unit = wide[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF)
        {
            ++i; // a pair
            continue;
        }
        if (unit >= 0xD800 && unit <= 0xDFFF)
        {
            return true;
        }
    }
    return false;
}

} // namespace Platform::WinString
