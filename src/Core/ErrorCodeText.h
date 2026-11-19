#pragma once

// An std::error_code's text as UTF-8 on every platform (#4308).
//
// On Windows std::system_category().message() is FormatMessageA in the process's ANSI code page: UTF-8 in
// TaskSmack.exe (its manifest sets activeCodePage), but cp1252 or cp932 in every binary without it (the tests,
// the UI training driver, the benchmarks), so "Accès refusé." arrives as code-page bytes in a UTF-8 string.
// std::filesystem reports its Win32 errors in that category. errorCodeText() sends those through
// FormatMessageW instead; every other category, and every code on Linux, keeps message().

#include <string>
#include <system_error>

#ifdef _WIN32
#include "Platform/Windows/WinString.h"
#endif

namespace Core
{

/// @p error's words, as UTF-8. On Windows a system_category code (a Win32 error) comes from FormatMessageW,
/// with the trailing period and line break trimmed; anything else is error.message().
[[nodiscard]] inline std::string errorCodeText(const std::error_code& error)
{
#ifdef _WIN32
    if (error.category() == std::system_category())
    {
        return Platform::WinString::win32ErrorText(static_cast<unsigned long>(error.value()));
    }
#endif
    return error.message();
}

} // namespace Core
