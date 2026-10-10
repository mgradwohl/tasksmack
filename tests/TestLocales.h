#pragma once

// The real en-US, de-DE and fr-FR display punctuation (#1648, slice E), as TaskSmack reads it at
// startup: the C++ locale's numpunct with the OS's UTF-8 separators. A test skips a locale the
// machine does not have.

#include "Core/LocaleSetup.h"
#include "UI/Format.h"

#include <exception>
#include <locale>
#include <optional>
#include <string>
#include <string_view>

namespace TestLocales
{

/// fr-FR's thousands separator: U+202F NARROW NO-BREAK SPACE, three UTF-8 bytes.
inline constexpr std::string_view NARROW_NBSP = "\xE2\x80\xAF";
/// U+00A0 NO-BREAK SPACE, which older glibc releases give fr_FR instead.
inline constexpr std::string_view NBSP = "\xC2\xA0";

/// The locale's name on this OS: "de-DE.UTF-8" on Windows, "de_DE.UTF-8" on Linux.
[[nodiscard]] inline auto osName(std::string_view language, std::string_view region) -> std::string
{
#ifdef _WIN32
    constexpr char SEPARATOR = '-';
#else
    constexpr char SEPARATOR = '_';
#endif
    std::string name(language);
    name += SEPARATOR;
    name += region;
    name += ".UTF-8";
    return name;
}

/// The display punctuation of @p language-@p region, or nullopt when the machine lacks the locale.
[[nodiscard]] inline auto punctuation(std::string_view language, std::string_view region) -> std::optional<UI::Format::NumericPunctuation>
{
    const std::string name = osName(language, region);
    try
    {
        const std::locale locale(name);
        return Core::LocaleSetup::numberPunctuation(locale, name);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace TestLocales
