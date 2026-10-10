#include "LocaleSetup.h"

#ifdef _WIN32
#include "Platform/Windows/WinString.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <array>

#include <windows.h>
#else
#include <langinfo.h>
#endif

#include <clocale>
#include <cstddef>
#include <exception>
#include <format>
#include <locale>
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

/// The user's C++ locale with a UTF-8 codeset. Windows: the same names the C locale tries. Linux:
/// the environment's locale, with the UTF-8 ctype the C locale settled on when it replaced one.
std::locale
buildUserLocale([[maybe_unused]] const CRuntime& crt, [[maybe_unused]] const CLocaleOutcome& cOutcome, std::vector<std::string>& warnings)
{
    std::string userName;
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

    const std::locale user = buildUserLocale(crt, cOutcome, summary.warnings);
    userLocaleStorage() = user;

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

std::string describe(const Summary& summary)
{
    const std::string acp = summary.activeCodePage != 0 ? std::to_string(summary.activeCodePage) : std::string("n/a");
    return std::format(
        R"(Locale: GetACP()={} C="{}" C++ global="{}" user="{}")", acp, summary.cLocale, summary.globalLocaleName, summary.userLocaleName);
}

} // namespace Core::LocaleSetup
