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
// The C++ side builds userLocale(), the user's locale for display (slice E formats numbers with
// it), and installs a global locale built from it. With NumericFacets::Classic the global locale
// keeps the user's collation, ctype and time facets but the classic numpunct/num_get/num_put, so
// streams and parsing stay locale-independent. NumericFacets::User keeps the user's numeric facets
// in the global locale too: UI::Format and the "{:L}" format specs read the global locale today, so
// TaskSmack installs User until slice E moves display formatting onto userLocale().
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
    User,    ///< The user's, so "{:L}" and UI::Format follow them (until slice E)
    Classic, ///< The classic locale's, so streams and parsing are locale-independent
};

/// The global locale built from @p user: @p user itself for NumericFacets::User, or @p user with the
/// classic locale's numeric facets for NumericFacets::Classic.
[[nodiscard]] std::locale makeGlobalLocale(const std::locale& user, NumericFacets numerics);

/// The locale state after initialize(), for the startup log line.
struct Summary
{
    unsigned activeCodePage = 0;  ///< GetACP() on Windows (65001 = UTF-8); 0 elsewhere
    std::string cLocale;          ///< setlocale(LC_ALL, nullptr)
    std::string globalLocaleName; ///< std::locale().name()
    std::string userLocaleName;   ///< userLocale().name()
    std::vector<std::string> warnings;
};

/// Sets the C and C++ locales. Call once, first thing in main, before any other thread starts and
/// before anything formats or parses text (setlocale and std::locale::global are not thread-safe).
/// Logs nothing, since logging is not up yet: log the result with describe() once it is.
const Summary& initialize(NumericFacets numerics);

/// The user's locale with a UTF-8 codeset, for display; the classic locale before initialize().
[[nodiscard]] const std::locale& userLocale() noexcept;

/// The one-line description of @p summary the startup log records.
[[nodiscard]] std::string describe(const Summary& summary);

} // namespace Core::LocaleSetup
