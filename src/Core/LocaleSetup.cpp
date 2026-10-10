#include "LocaleSetup.h"

#include <SDL3/SDL_stdinc.h>

#ifdef _WIN32
#include "Platform/Windows/WinString.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <array>

#include <windows.h>
#else
#include <langinfo.h>
#include <locale.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers) - newlocale/freelocale are POSIX, not in <clocale>
#endif

#include <algorithm>
#include <clocale>
#include <cstddef>
#include <exception>
#include <format>
#include <locale>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core::LocaleSetup
{

namespace
{

/// The locale storage behind userLocale(): written once by initialize(), before any other thread.
std::locale& userLocaleStorage()
{
    static std::locale locale = std::locale::classic();
    return locale;
}

/// The storage behind userNumberPunctuation(), written with userLocaleStorage().
NumberPunctuation& userPunctuationStorage()
{
    static NumberPunctuation punctuation;
    return punctuation;
}

[[nodiscard]] bool isAscii(const char c) noexcept
{
    return static_cast<unsigned char>(c) < 0x80U;
}

/// The separators an OS locale database gives, as UTF-8; empty strings for what it does not say.
struct OsPunctuation
{
    std::string decimalPoint;
    std::string thousandsSep;
    std::string grouping; ///< numpunct::grouping() form; Linux only
};

/// The OS's punctuation for @p name (see numberPunctuation()); nullopt for "C"/"POSIX" or a name the
/// OS does not know.
std::optional<OsPunctuation> osPunctuation(const std::string_view name)
{
    // "language-REGION.codeset" / "language_TERRITORY.codeset@modifier": the OS lookups want the
    // locale, not the codeset.
    const std::string_view base = name.substr(0, name.find('.'));
    if (base == "C" || base == "POSIX")
    {
        return std::nullopt;
    }
#ifdef _WIN32
    const std::wstring wideName = Platform::WinString::utf8ToWide(base);
    const wchar_t* localeName = base.empty() ? LOCALE_NAME_USER_DEFAULT : wideName.c_str();
    const auto read = [localeName](const LCTYPE type) -> std::optional<std::string>
    {
        // LOCALE_SDECIMAL and LOCALE_STHOUSAND are at most three characters plus the terminator.
        std::array<wchar_t, 16> text{};
        if (GetLocaleInfoEx(localeName, type, text.data(), static_cast<int>(text.size())) == 0)
        {
            return std::nullopt;
        }
        return Platform::WinString::wideToUtf8(text.data());
    };
    std::optional<std::string> decimalPoint = read(LOCALE_SDECIMAL);
    std::optional<std::string> thousandsSep = read(LOCALE_STHOUSAND);
    if (!decimalPoint || !thousandsSep)
    {
        return std::nullopt;
    }
    return OsPunctuation{.decimalPoint = std::move(*decimalPoint), .thousandsSep = std::move(*thousandsSep), .grouping = {}};
#else
    // newlocale() reads "" from the environment, as setlocale() does.
    const std::string localeName(name);
    // NOLINTNEXTLINE(misc-include-cleaner) - locale_t comes from <locale.h> (POSIX) through glibc's internal headers
    locale_t locale = newlocale(LC_NUMERIC_MASK, localeName.c_str(), static_cast<locale_t>(nullptr));
    if (locale == static_cast<locale_t>(nullptr))
    {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(misc-include-cleaner) - nl_item comes from <langinfo.h> (POSIX) through glibc's <nl_types.h>
    const auto read = [locale](const nl_item item) -> std::string
    {
        const char* text = nl_langinfo_l(item, locale);
        return text != nullptr ? std::string(text) : std::string();
    };
    OsPunctuation punctuation{.decimalPoint = read(RADIXCHAR), .thousandsSep = read(THOUSEP), .grouping = {}};
#ifdef GROUPING
    punctuation.grouping = read(GROUPING);
#endif
    freelocale(locale);
    return punctuation;
#endif
}

/// Makes @p punctuation valid UTF-8 and self-consistent: a lone non-ASCII separator byte (the
/// first byte of a multibyte character, or a legacy code page's no-break space) becomes U+00A0, a
/// non-ASCII decimal mark becomes '.' (',' when '.' groups), and no separator means no grouping.
void sanitize(NumberPunctuation& punctuation)
{
    if (punctuation.thousandsSep.size() == 1 && !isAscii(punctuation.thousandsSep.front()))
    {
        punctuation.thousandsSep = "\u00A0";
    }
    if (!isAscii(punctuation.decimalPoint) || punctuation.decimalPoint == '\0')
    {
        punctuation.decimalPoint = (punctuation.thousandsSep == ".") ? ',' : '.';
    }
    if (punctuation.thousandsSep.empty() || punctuation.thousandsSep.front() == '\0' || punctuation.grouping.empty())
    {
        punctuation.thousandsSep.clear();
        punctuation.grouping.clear();
    }
}

/// Tries @p names in order with setlocale(@p category); the first one accepted, or empty.
std::string setFirst(const CRuntime& crt, const int category, const std::vector<std::string>& names)
{
    for (const std::string& name : names)
    {
        if (crt.setLocale(category, name.c_str()) != nullptr)
        {
            return name;
        }
    }
    return {};
}

/// Forces LC_NUMERIC to "C", noting a failure (which the C runtime should never report).
void forceClassicNumeric(const CRuntime& crt, std::vector<std::string>& warnings)
{
    if (crt.setLocale(LC_NUMERIC, "C") == nullptr)
    {
        warnings.emplace_back("setlocale(LC_NUMERIC, \"C\") failed");
    }
}

auto joined(const std::vector<std::string>& names) -> std::string
{
    std::string text;
    for (const std::string& name : names)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += '"';
        text += name;
        text += '"';
    }
    return text;
}

// The real C runtime behind CRuntime.
const char* realSetLocale(const int category, const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - initialize() runs once at startup before any thread starts
    return std::setlocale(category, name);
}

std::string realUserLocaleName()
{
#ifdef _WIN32
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> name{};
    if (GetUserDefaultLocaleName(name.data(), static_cast<int>(name.size())) == 0)
    {
        return {};
    }
    return Platform::WinString::wideToUtf8(name.data());
#else
    return {};
#endif
}

// NOLINTNEXTLINE(modernize-use-string-view) - the signature CRuntime::codeset takes; nl_langinfo's buffer is reused
std::string realCodeset()
{
#ifdef _WIN32
    return {};
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - initialize() runs once at startup before any thread starts
    const char* codeset = nl_langinfo(CODESET);
    return codeset != nullptr ? std::string(codeset) : std::string();
#endif
}

} // namespace

bool isUtf8Codeset(const std::string_view codeset) noexcept
{
    // "utf8" once '-' and '_' are dropped, compared ASCII case-insensitively.
    constexpr std::string_view EXPECTED = "utf8";
    std::size_t matched = 0;
    std::size_t index = 0;
    while (index < codeset.size())
    {
        char c = codeset[index];
        ++index;
        if (c == '-' || c == '_')
        {
            continue;
        }
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (matched == EXPECTED.size() || c != EXPECTED[matched])
        {
            return false;
        }
        ++matched;
    }
    return matched == EXPECTED.size();
}

std::vector<std::string> windowsCLocaleCandidates(const std::string_view userLocaleName)
{
    std::vector<std::string> names;
    names.reserve(3);
    if (!userLocaleName.empty())
    {
        names.emplace_back(std::string(userLocaleName) + ".UTF-8");
    }
    names.emplace_back(".UTF-8");
    names.emplace_back("C.UTF-8");
    return names;
}

std::vector<std::string> linuxCtypeCandidates(const std::string_view currentName)
{
    std::vector<std::string> names;
    names.reserve(3);
    // "language_TERRITORY.codeset@modifier": keep language_TERRITORY.
    const std::string_view language = currentName.substr(0, currentName.find_first_of(".@"));
    if (!language.empty() && language != "C" && language != "POSIX")
    {
        names.emplace_back(std::string(language) + ".UTF-8");
    }
    names.emplace_back("C.UTF-8");
    names.emplace_back("en_US.UTF-8");
    return names;
}

CLocaleOutcome applyWindowsCLocale(const CRuntime& crt)
{
    CLocaleOutcome outcome;
    const std::vector<std::string> names = windowsCLocaleCandidates(crt.userLocaleName());
    outcome.chosen = setFirst(crt, LC_ALL, names);
    if (outcome.chosen.empty())
    {
        outcome.warnings.emplace_back(std::format("no UTF-8 C locale is available (tried {})", joined(names)));
    }
    else if (outcome.chosen != names.front())
    {
        outcome.warnings.emplace_back(std::format(R"(C locale "{}" is not available; using "{}")", names.front(), outcome.chosen));
    }
    forceClassicNumeric(crt, outcome.warnings);
    return outcome;
}

CLocaleOutcome applyLinuxCLocale(const CRuntime& crt)
{
    CLocaleOutcome outcome;
    if (crt.setLocale(LC_ALL, "") == nullptr)
    {
        outcome.warnings.emplace_back("setlocale(LC_ALL, \"\") failed: the environment names a locale that is not installed");
    }
    const char* ctype = crt.setLocale(LC_CTYPE, nullptr);
    const std::string userCtype = ctype != nullptr ? std::string(ctype) : std::string();
    if (isUtf8Codeset(crt.codeset()))
    {
        outcome.chosen = userCtype;
    }
    else
    {
        const std::vector<std::string> names = linuxCtypeCandidates(userCtype);
        outcome.chosen = setFirst(crt, LC_CTYPE, names);
        if (outcome.chosen.empty())
        {
            outcome.warnings.emplace_back(
                std::format("LC_CTYPE \"{}\" is not UTF-8 and no UTF-8 locale is available (tried {})", userCtype, joined(names)));
        }
        else
        {
            outcome.replacedCtype = true;
            outcome.warnings.emplace_back(std::format(R"(LC_CTYPE "{}" is not UTF-8; using "{}")", userCtype, outcome.chosen));
        }
    }
    forceClassicNumeric(crt, outcome.warnings);
    return outcome;
}

std::locale makeFirstLocale(const std::vector<std::string>& names, std::string& chosenName, std::vector<std::string>& warnings)
{
    for (const std::string& name : names)
    {
        try
        {
            const std::locale locale(name);
            chosenName = name;
            return locale;
        }
        catch (const std::exception&) // NOLINT(bugprone-empty-catch) - an unknown name: try the next one
        {}
    }
    warnings.emplace_back(std::format("no C++ locale is available (tried {}); using the classic \"C\" locale", joined(names)));
    chosenName = "C";
    return std::locale::classic();
}

std::locale makeGlobalLocale(const std::locale& user, const NumericFacets numerics)
{
    if (numerics == NumericFacets::User)
    {
        return user;
    }
    return {user, std::locale::classic(), std::locale::numeric};
}

namespace
{

/// The user's C++ locale with a UTF-8 codeset, and in @p userName the name it was made from. Windows:
/// the same names the C locale tries. Linux: the environment's locale (""), with the UTF-8 ctype the
/// C locale settled on when it replaced one.
std::locale buildUserLocale([[maybe_unused]] const CRuntime& crt,
                            [[maybe_unused]] const CLocaleOutcome& cOutcome,
                            std::string& userName,
                            std::vector<std::string>& warnings)
{
#ifdef _WIN32
    return makeFirstLocale(windowsCLocaleCandidates(crt.userLocaleName()), userName, warnings);
#else
    const std::locale user = makeFirstLocale({""}, userName, warnings);
    if (!cOutcome.replacedCtype)
    {
        return user;
    }
    try
    {
        return {user, std::locale(cOutcome.chosen), std::locale::ctype};
    }
    catch (const std::exception& e)
    {
        warnings.emplace_back(std::format(R"(C++ ctype locale "{}" is not available: {})", cOutcome.chosen, e.what()));
        return user;
    }
#endif
}

} // namespace

const Summary& initialize(const NumericFacets numerics)
{
    static Summary summary;

    const CRuntime crt{.setLocale = &realSetLocale, .userLocaleName = &realUserLocaleName, .codeset = &realCodeset};

    // The C locale first: on Linux the C++ user locale takes its LC_CTYPE fallback from it.
#ifdef _WIN32
    CLocaleOutcome cOutcome = applyWindowsCLocale(crt);
#else
    CLocaleOutcome cOutcome = applyLinuxCLocale(crt);
#endif
    summary.warnings = std::move(cOutcome.warnings);

    std::string userName;
    const std::locale user = buildUserLocale(crt, cOutcome, userName, summary.warnings);

    // The display locale: the user's, or the TASKSMACK_LOCALE test hook's when it is set.
    std::locale display = user;
    std::string displayName = userName;
    // SDL_getenv: UTF-8 on Windows too, and no CRT deprecation; it needs no SDL_Init.
    if (const char* overrideValue = SDL_getenv(LOCALE_OVERRIDE_ENV); overrideValue != nullptr && *overrideValue != '\0')
    {
        std::string overrideName;
        std::vector<std::string> overrideWarnings;
        const std::vector<std::string> names = localeOverrideCandidates(overrideValue);
        const std::locale overridden = makeFirstLocale(names, overrideName, overrideWarnings);
        if (overrideName != "C" || std::ranges::find(names, "C") != names.end())
        {
            display = overridden;
            displayName = overrideName;
            summary.userLocaleOverridden = true;
        }
        else
        {
            summary.warnings.emplace_back(std::format(R"({}="{}" names no available locale; ignored)", LOCALE_OVERRIDE_ENV, overrideValue));
        }
    }
    userLocaleStorage() = display;
    userPunctuationStorage() = numberPunctuation(display, displayName);

    // std::locale::global() also calls setlocale(LC_ALL, name) when the locale has a name, which
    // would undo LC_NUMERIC "C": put the C locale back as it was.
    const char* cBefore = crt.setLocale(LC_ALL, nullptr);
    const std::string cLocale = cBefore != nullptr ? std::string(cBefore) : std::string();
    std::locale::global(makeGlobalLocale(user, numerics));
    if (!cLocale.empty() && crt.setLocale(LC_ALL, cLocale.c_str()) == nullptr)
    {
        summary.warnings.emplace_back(std::format("could not restore the C locale \"{}\"", cLocale));
    }

#ifdef _WIN32
    summary.activeCodePage = GetACP();
#endif
    const char* cAfter = crt.setLocale(LC_ALL, nullptr);
    summary.cLocale = cAfter != nullptr ? std::string(cAfter) : std::string();
    summary.globalLocaleName = std::locale().name();
    summary.userLocaleName = userLocaleStorage().name();
    return summary;
}

const std::locale& userLocale() noexcept
{
    return userLocaleStorage();
}

const NumberPunctuation& userNumberPunctuation() noexcept
{
    return userPunctuationStorage();
}

NumberPunctuation numberPunctuation(const std::locale& locale)
{
    NumberPunctuation punctuation;
    try
    {
        const auto& facet = std::use_facet<std::numpunct<char>>(locale);
        punctuation.decimalPoint = facet.decimal_point();
        punctuation.thousandsSep = std::string(1, facet.thousands_sep());
        punctuation.grouping = facet.grouping();
    }
    catch (const std::exception&)
    {
        return {}; // No numpunct facet: the classic punctuation
    }
    sanitize(punctuation);
    return punctuation;
}

NumberPunctuation numberPunctuation(const std::locale& locale, const std::string_view osLocaleName)
{
    NumberPunctuation punctuation = numberPunctuation(locale);
    const std::optional<OsPunctuation> os = osPunctuation(osLocaleName);
    if (!os)
    {
        return punctuation;
    }
    if (os->decimalPoint.size() == 1 && isAscii(os->decimalPoint.front()))
    {
        punctuation.decimalPoint = os->decimalPoint.front();
    }
    // The facet decides whether and how digits are grouped; the OS spells the separator. A C++
    // library that could not narrow a multibyte separator may have dropped grouping (libstdc++):
    // the OS's grouping stands in then.
    if (punctuation.grouping.empty() && !os->grouping.empty() && !os->thousandsSep.empty())
    {
        punctuation.grouping = os->grouping;
        punctuation.thousandsSep = os->thousandsSep;
    }
    else if (!punctuation.grouping.empty() && !os->thousandsSep.empty())
    {
        punctuation.thousandsSep = os->thousandsSep;
    }
    sanitize(punctuation);
    return punctuation;
}

std::vector<std::string> localeOverrideCandidates(const std::string_view value)
{
    std::vector<std::string> names;
    if (value.empty())
    {
        return names;
    }
    if (!value.contains('.'))
    {
        names.emplace_back(std::string(value) + ".UTF-8");
    }
    names.emplace_back(value);
    return names;
}

std::string describe(const Summary& summary)
{
    const std::string acp = summary.activeCodePage != 0 ? std::to_string(summary.activeCodePage) : std::string("n/a");
    return std::format(R"(Locale: GetACP()={} C="{}" C++ global="{}" user="{}"{})",
                       acp,
                       summary.cLocale,
                       summary.globalLocaleName,
                       summary.userLocaleName,
                       summary.userLocaleOverridden ? " (TASKSMACK_LOCALE)" : "");
}

} // namespace Core::LocaleSetup
