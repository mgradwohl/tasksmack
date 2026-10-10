#pragma once

// The process's C and C++ locales, set once at startup (#1648, slice B): the user's regional
// settings with a UTF-8 codeset, and LC_NUMERIC kept at "C" so nothing the C runtime parses or
// prints depends on the user's decimal mark.
//
//   Windows  setlocale(LC_ALL, "<GetUserDefaultLocaleName()>.UTF-8"), else ".UTF-8", else "C.UTF-8"
//   Linux    setlocale(LC_ALL, ""); if its codeset is not UTF-8, LC_CTYPE becomes the same language
//            with ".UTF-8", else "C.UTF-8", else "en_US.UTF-8"
//   Both     then setlocale(LC_NUMERIC, "C")
//
// The C++ side builds userLocale(), the user's locale for display, and its userNumberPunctuation()
// (the decimal mark, digit grouping and thousands separator UI::Format prints numbers with, #1648
// slice E), and installs a global locale built from it. TaskSmack uses NumericFacets::Classic: the
// global locale keeps the user's collation, ctype and time facets but the classic
// numpunct/num_get/num_put, so streams, "{:L}" and parsing stay locale-independent and display
// formatting never reads the global locale. NumericFacets::User keeps the user's numeric facets in
// the global locale too; nothing in TaskSmack uses it now.
//
// TASKSMACK_LOCALE (a test hook, read only when set) replaces the locale userLocale() and
// userNumberPunctuation() are built from, e.g. "de-DE" (Windows) or "de_DE" (Linux), so a
// screenshot on an en-US machine can show decimal commas. The C locale and the global C++ locale
// are not changed by it.
//
// The selection logic takes its C runtime calls through CRuntime, so the fallback order is
// unit-tested (tests/Core/test_LocaleSetup.cpp) without changing the test process's locale.
// initialize() is the only part that uses the real C runtime and the global locale.

#include <cstdint>
#include <locale>
#include <string>
#include <string_view>
#include <vector>

namespace Core::LocaleSetup
{

/// The C runtime calls the C-locale selection makes.
struct CRuntime
{
    /// setlocale(): the locale's name on success, nullptr when @p name is not available.
    const char* (*setLocale)(int category, const char* name) = nullptr;
    /// The user's locale name, as GetUserDefaultLocaleName() gives it (for example "de-DE"), in
    /// UTF-8; empty when unknown. Used on Windows only.
    std::string (*userLocaleName)() = nullptr;
    /// nl_langinfo(CODESET) for the current LC_CTYPE (for example "UTF-8"). Used on Linux only.
    std::string (*codeset)() = nullptr;
};

/// What a C-locale selection did: the candidate setlocale accepted (empty when none was), and
/// anything worth a warning in the log.
struct CLocaleOutcome
{
    std::string chosen;
    std::vector<std::string> warnings;
    /// Linux: the user's LC_CTYPE was not UTF-8 and chosen replaced it.
    bool replacedCtype = false;
};

/// Whether @p codeset names UTF-8 ("UTF-8", "utf8", any case).
[[nodiscard]] bool isUtf8Codeset(std::string_view codeset) noexcept;

/// The LC_ALL names to try on Windows, in order: "<userLocaleName>.UTF-8" (when the name is not
/// empty), ".UTF-8" (the user's default locale with a UTF-8 codeset), "C.UTF-8".
[[nodiscard]] std::vector<std::string> windowsCLocaleCandidates(std::string_view userLocaleName);

/// The LC_CTYPE names to try on Linux when the user's locale (@p currentName, setlocale's name for
/// LC_CTYPE, for example "de_DE.ISO-8859-1") has a non-UTF-8 codeset: the same language with
/// ".UTF-8" (not for "C"/"POSIX"), then "C.UTF-8", then "en_US.UTF-8".
[[nodiscard]] std::vector<std::string> linuxCtypeCandidates(std::string_view currentName);

/// Sets the Windows C locale through @p crt: LC_ALL from windowsCLocaleCandidates(), then
/// LC_NUMERIC "C".
[[nodiscard]] CLocaleOutcome applyWindowsCLocale(const CRuntime& crt);

/// Sets the Linux C locale through @p crt: LC_ALL "", LC_CTYPE from linuxCtypeCandidates() when
/// its codeset is not UTF-8, then LC_NUMERIC "C". chosen is the user's LC_CTYPE name, or the
/// fallback that replaced it.
[[nodiscard]] CLocaleOutcome applyLinuxCLocale(const CRuntime& crt);

/// The first of @p names that std::locale accepts, with its name in @p chosenName; the classic
/// locale ("C") and a warning in @p warnings when none is (std::locale throws for unknown names).
[[nodiscard]] std::locale
makeFirstLocale(const std::vector<std::string>& names, std::string& chosenName, std::vector<std::string>& warnings);

/// Which numeric facets (numpunct, num_get, num_put) the global C++ locale takes.
enum class NumericFacets : std::uint8_t
{
    User,    ///< The user's, so "{:L}" and streams follow them
    Classic, ///< The classic locale's, so streams and parsing are locale-independent (TaskSmack's choice)
};

/// The global locale built from @p user: @p user itself for NumericFacets::User, or @p user with the
/// classic locale's numeric facets for NumericFacets::Classic.
[[nodiscard]] std::locale makeGlobalLocale(const std::locale& user, NumericFacets numerics);

/// How a locale writes numbers for display: what std::format("{:L}") puts between the digits, except
/// that the thousands separator is the whole UTF-8 sequence. numpunct<char>::thousands_sep() is one
/// byte, so a locale whose separator is not ASCII -- fr-FR's narrow no-break space U+202F, de-CH's
/// U+2019 -- gets its first byte from the C++ library, half a UTF-8 character that ImGui shows as "?".
struct NumberPunctuation
{
    char decimalPoint = '.';  ///< Always one ASCII character
    std::string thousandsSep; ///< UTF-8, one or more bytes; empty when the locale does not group digits
    std::string grouping;     ///< numpunct::grouping(): group sizes from the right; empty for no grouping

    bool operator==(const NumberPunctuation&) const = default;
};

/// @p locale's numpunct facet as NumberPunctuation. A non-ASCII separator byte (see above) becomes a
/// no-break space U+00A0 and a non-ASCII decimal mark becomes '.' (',' when '.' groups the digits),
/// so the result is always valid UTF-8.
[[nodiscard]] NumberPunctuation numberPunctuation(const std::locale& locale);

/// numberPunctuation(@p locale), with the separators the OS gives for @p osLocaleName in full: on
/// Windows GetLocaleInfoEx(LOCALE_STHOUSAND/LOCALE_SDECIMAL) for the name up to its '.' ("fr-FR" for
/// "fr-FR.UTF-8"; "" or ".UTF-8" is the user's default locale), with the user's Regional format
/// customizations; on Linux nl_langinfo_l(THOUSEP/RADIXCHAR) for newlocale(LC_NUMERIC_MASK, name)
/// ("" is the environment's LC_NUMERIC). @p osLocaleName "C" or "POSIX", or a name the OS does not
/// know, gives numberPunctuation(@p locale).
[[nodiscard]] NumberPunctuation numberPunctuation(const std::locale& locale, std::string_view osLocaleName);

/// The test hook's environment variable: see the top of this file.
inline constexpr const char* LOCALE_OVERRIDE_ENV = "TASKSMACK_LOCALE";

/// The names to try for a TASKSMACK_LOCALE value, in order: "<value>.UTF-8" (unless it already names
/// a codeset), then the value itself. Empty for an empty value.
[[nodiscard]] std::vector<std::string> localeOverrideCandidates(std::string_view value);

/// The locale state after initialize(), for the startup log line.
struct Summary
{
    unsigned activeCodePage = 0;       ///< GetACP() on Windows (65001 = UTF-8); 0 elsewhere
    std::string cLocale;               ///< setlocale(LC_ALL, nullptr)
    std::string globalLocaleName;      ///< std::locale().name()
    std::string userLocaleName;        ///< userLocale().name()
    bool userLocaleOverridden = false; ///< userLocale() came from TASKSMACK_LOCALE
    std::vector<std::string> warnings;
};

/// Sets the C and C++ locales. Call once, first thing in main, before any other thread starts and
/// before anything formats or parses text (setlocale and std::locale::global are not thread-safe).
/// Logs nothing, since logging is not up yet: log the result with describe() once it is.
const Summary& initialize(NumericFacets numerics);

/// The user's locale with a UTF-8 codeset, for display; the classic locale before initialize().
[[nodiscard]] const std::locale& userLocale() noexcept;

/// userLocale()'s numberPunctuation() with the OS's separators, read once by initialize(): what
/// UI::Format prints displayed numbers with. The classic locale's ('.', no grouping) before it.
[[nodiscard]] const NumberPunctuation& userNumberPunctuation() noexcept;

/// The one-line description of @p summary the startup log records.
[[nodiscard]] std::string describe(const Summary& summary);

} // namespace Core::LocaleSetup
