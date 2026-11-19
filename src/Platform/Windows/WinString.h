#pragma once

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

} // namespace Platform::WinString
