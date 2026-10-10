/// @file test_LocaleSetup.cpp
/// @brief The startup C/C++ locale selection (#1648, slice B), through a fake C runtime

#include "Core/LocaleSetup.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
#include <locale>
#include <string>
#include <utility>
#include <vector>

namespace Core::LocaleSetup
{
namespace
{

/// A fake C runtime: setlocale accepts only the names in `available` (plus "" and "C"), and records
/// every call. Plain function pointers can't capture, so the state is a test-wide singleton reset by
/// the fixture.
struct FakeCrt
{
    std::vector<std::string> available;
    std::string userName;
    std::string codeset = "UTF-8";
    std::string envCtype = "en_US.UTF-8"; // what setlocale(LC_ALL, "") selects
    std::string ctype = "C";              // the current LC_CTYPE name
    std::vector<std::pair<int, std::string>> calls;
};

FakeCrt& fake()
{
    static FakeCrt state; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - fake C runtime state
    return state;
}

const char* fakeSetLocale(const int category, const char* name)
{
    FakeCrt& crt = fake();
    if (name == nullptr)
    {
        return crt.ctype.c_str();
    }
    crt.calls.emplace_back(category, name);
    const std::string requested(name);
    if (requested.empty())
    {
        crt.ctype = crt.envCtype;
        return crt.ctype.c_str();
    }
    if (requested != "C" && std::ranges::find(crt.available, requested) == crt.available.end())
    {
        return nullptr;
    }
    if (category == LC_ALL || category == LC_CTYPE)
    {
        crt.ctype = requested;
    }
    return crt.ctype.c_str();
}

std::string fakeUserLocaleName()
{
    return fake().userName;
}

std::string fakeCodeset()
{
    return fake().codeset;
}

constexpr CRuntime FAKE_CRT{.setLocale = &fakeSetLocale, .userLocaleName = &fakeUserLocaleName, .codeset = &fakeCodeset};

class LocaleSetupTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeCrt{};
    }
};

TEST(LocaleSetupCodesetTest, RecognisesUtf8Spellings)
{
    EXPECT_TRUE(isUtf8Codeset("UTF-8"));
    EXPECT_TRUE(isUtf8Codeset("utf8"));
    EXPECT_TRUE(isUtf8Codeset("Utf_8"));
    EXPECT_FALSE(isUtf8Codeset("ISO-8859-1"));
    EXPECT_FALSE(isUtf8Codeset("ANSI_X3.4-1968"));
    EXPECT_FALSE(isUtf8Codeset(""));
}

TEST(LocaleSetupCandidatesTest, WindowsTriesTheUserLocaleThenTheDefaultsWithUtf8)
{
    EXPECT_EQ(windowsCLocaleCandidates("de-DE"), (std::vector<std::string>{"de-DE.UTF-8", ".UTF-8", "C.UTF-8"}));
    EXPECT_EQ(windowsCLocaleCandidates(""), (std::vector<std::string>{".UTF-8", "C.UTF-8"}));
}

TEST(LocaleSetupCandidatesTest, LinuxKeepsTheLanguageAndSwapsTheCodeset)
{
    EXPECT_EQ(linuxCtypeCandidates("de_DE.ISO-8859-1"), (std::vector<std::string>{"de_DE.UTF-8", "C.UTF-8", "en_US.UTF-8"}));
    EXPECT_EQ(linuxCtypeCandidates("fr_FR@euro"), (std::vector<std::string>{"fr_FR.UTF-8", "C.UTF-8", "en_US.UTF-8"}));
    EXPECT_EQ(linuxCtypeCandidates("C"), (std::vector<std::string>{"C.UTF-8", "en_US.UTF-8"}));
    EXPECT_EQ(linuxCtypeCandidates("POSIX"), (std::vector<std::string>{"C.UTF-8", "en_US.UTF-8"}));
}

TEST_F(LocaleSetupTest, WindowsUsesTheUserLocaleWithUtf8AndClassicNumbers)
{
    fake().userName = "de-DE";
    fake().available = {"de-DE.UTF-8", ".UTF-8", "C.UTF-8"};
    const CLocaleOutcome outcome = applyWindowsCLocale(FAKE_CRT);
    EXPECT_EQ(outcome.chosen, "de-DE.UTF-8");
    EXPECT_TRUE(outcome.warnings.empty());
    ASSERT_FALSE(fake().calls.empty());
    EXPECT_EQ(fake().calls.back(), (std::pair<int, std::string>{LC_NUMERIC, "C"}));
}

TEST_F(LocaleSetupTest, WindowsFallsBackToTheDefaultUtf8Locale)
{
    fake().userName = "xx-YY";
    fake().available = {".UTF-8", "C.UTF-8"};
    const CLocaleOutcome outcome = applyWindowsCLocale(FAKE_CRT);
    EXPECT_EQ(outcome.chosen, ".UTF-8");
    EXPECT_EQ(outcome.warnings.size(), 1U);
}

TEST_F(LocaleSetupTest, WindowsFallsBackToCUtf8)
{
    fake().userName = "xx-YY";
    fake().available = {"C.UTF-8"};
    EXPECT_EQ(applyWindowsCLocale(FAKE_CRT).chosen, "C.UTF-8");
}

TEST_F(LocaleSetupTest, WindowsWithNoUtf8LocaleWarnsAndStillForcesClassicNumbers)
{
    fake().userName = "de-DE";
    const CLocaleOutcome outcome = applyWindowsCLocale(FAKE_CRT);
    EXPECT_TRUE(outcome.chosen.empty());
    EXPECT_EQ(outcome.warnings.size(), 1U);
    EXPECT_EQ(fake().calls.back(), (std::pair<int, std::string>{LC_NUMERIC, "C"}));
}

TEST_F(LocaleSetupTest, LinuxKeepsAUtf8UserLocale)
{
    fake().envCtype = "de_DE.UTF-8";
    const CLocaleOutcome outcome = applyLinuxCLocale(FAKE_CRT);
    EXPECT_EQ(outcome.chosen, "de_DE.UTF-8");
    EXPECT_FALSE(outcome.replacedCtype);
    EXPECT_TRUE(outcome.warnings.empty());
    EXPECT_EQ(fake().calls.front(), (std::pair<int, std::string>{LC_ALL, ""}));
    EXPECT_EQ(fake().calls.back(), (std::pair<int, std::string>{LC_NUMERIC, "C"}));
}

TEST_F(LocaleSetupTest, LinuxReplacesANonUtf8CtypeWithTheSameLanguage)
{
    fake().envCtype = "de_DE.ISO-8859-1";
    fake().codeset = "ISO-8859-1";
    fake().available = {"de_DE.UTF-8", "C.UTF-8"};
    const CLocaleOutcome outcome = applyLinuxCLocale(FAKE_CRT);
    EXPECT_EQ(outcome.chosen, "de_DE.UTF-8");
    EXPECT_TRUE(outcome.replacedCtype);
    EXPECT_EQ(outcome.warnings.size(), 1U);
}

TEST_F(LocaleSetupTest, LinuxPosixLocaleFallsBackToCUtf8ThenEnUs)
{
    fake().envCtype = "C";
    fake().codeset = "ANSI_X3.4-1968";
    fake().available = {"en_US.UTF-8"};
    const CLocaleOutcome outcome = applyLinuxCLocale(FAKE_CRT);
    EXPECT_EQ(outcome.chosen, "en_US.UTF-8");
    EXPECT_TRUE(outcome.replacedCtype);
    EXPECT_EQ(fake().calls.back(), (std::pair<int, std::string>{LC_NUMERIC, "C"}));
}

TEST(LocaleSetupCppTest, UnknownLocaleNamesFallBackToClassicWithAWarning)
{
    std::string chosen;
    std::vector<std::string> warnings;
    const std::locale locale = makeFirstLocale({"no-such-locale.NOPE"}, chosen, warnings);
    EXPECT_EQ(chosen, "C");
    EXPECT_EQ(locale, std::locale::classic());
    EXPECT_EQ(warnings.size(), 1U);
}

TEST(LocaleSetupCppTest, ClassicGlobalKeepsTheDotWhileTheUserLocaleUsesTheComma)
{
#ifdef _WIN32
    const std::vector<std::string> names{"de-DE.UTF-8"};
#else
    const std::vector<std::string> names{"de_DE.UTF-8"};
#endif
    std::string chosen;
    std::vector<std::string> warnings;
    const std::locale user = makeFirstLocale(names, chosen, warnings);
    if (chosen == "C")
    {
        GTEST_SKIP() << names.front() << " is not available on this machine";
    }
    EXPECT_EQ(std::use_facet<std::numpunct<char>>(user).decimal_point(), ',');

    const std::locale global = makeGlobalLocale(user, NumericFacets::Classic);
    EXPECT_EQ(std::use_facet<std::numpunct<char>>(global).decimal_point(), '.');
    EXPECT_TRUE(std::use_facet<std::numpunct<char>>(global).grouping().empty());

    const std::locale userNumbers = makeGlobalLocale(user, NumericFacets::User);
    EXPECT_EQ(std::use_facet<std::numpunct<char>>(userNumbers).decimal_point(), ',');
}

TEST(LocaleSetupDescribeTest, NamesEveryField)
{
    const Summary summary{
        .activeCodePage = 65001,
        .cLocale = "de-DE.UTF-8",
        .globalLocaleName = "*",
        .userLocaleName = "de-DE.UTF-8",
        .warnings = {},
    };
    EXPECT_EQ(describe(summary), R"(Locale: GetACP()=65001 C="de-DE.UTF-8" C++ global="*" user="de-DE.UTF-8")");
    EXPECT_EQ(describe(Summary{}), R"(Locale: GetACP()=n/a C="" C++ global="" user="")");
}

} // namespace
} // namespace Core::LocaleSetup
