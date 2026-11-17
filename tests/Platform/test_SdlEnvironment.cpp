// Tests for SdlEnvironment (#4103): the SDL variables an elevated TaskSmack.exe removes from its environment before
// any SDL call, so SDL loads no DLL a hint from the less-privileged side names (SDL_OPENGL_LIBRARY, SDL3_DYNAMIC_API).
// Through a fake environment block for every case, then on this process's own: only its environment changes, and
// each test puts back every SDL variable it found.

#include "Platform/Windows/SdlEnvironment.h"
#include "Platform/Windows/WinString.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows::SdlEnvironment
{
namespace
{

/// The whole of @p text, its embedded nulls included but not the literal's own terminator: a block as
/// GetEnvironmentStringsW() gives one.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - a literal's length, embedded nulls included
template<std::size_t N> [[nodiscard]] constexpr std::wstring_view block(const wchar_t (&text)[N]) noexcept
{
    return {static_cast<const wchar_t*>(text), N - 1};
}

// As GetEnvironmentStringsW() gives it: a per-drive entry, the user's variables, SDL's in mixed case, and near misses.
constexpr std::wstring_view BLOCK = block(L"=C:=C:\\Users\\me\0"
                                          L"PATH=C:\\Windows\\System32\0"
                                          L"SDL_OPENGL_LIBRARY=C:\\Users\\me\\AppData\\Local\\Temp\\opengl32.dll\0"
                                          L"sdl3_dynamic_api=C:\\Users\\me\\SDL3.dll\0"
                                          L"SDL_VIDEODRIVER=dummy\0"
                                          L"SDLX=1\0"
                                          L"MYSDL_THING=1\0"
                                          L"TASKSMACK_CONFIG_DIR=C:\\Users\\me\\cfg\0"
                                          L"Sdl_Egl_Library=libEGL.dll\0"
                                          L"\0");

/// The SDL variables in BLOCK, in order.
[[nodiscard]] std::vector<std::wstring> sdlNames()
{
    return {L"SDL_OPENGL_LIBRARY", L"sdl3_dynamic_api", L"SDL_VIDEODRIVER", L"Sdl_Egl_Library"};
}

TEST(SdlEnvironmentTest, SdlVariablesAreSdlUnderscoreOrSdlDigitsUnderscoreInAnyCase)
{
    EXPECT_TRUE(isSdlVariable(L"SDL_OPENGL_LIBRARY"));
    EXPECT_TRUE(isSdlVariable(L"SDL_EGL_LIBRARY"));
    EXPECT_TRUE(isSdlVariable(L"SDL_VIDEODRIVER")); // SDL2's name, which SDL 3 still reads
    EXPECT_TRUE(isSdlVariable(L"SDL3_DYNAMIC_API"));
    EXPECT_TRUE(isSdlVariable(L"SDL2_X"));
    EXPECT_TRUE(isSdlVariable(L"sdl_opengl_library"));
    EXPECT_TRUE(isSdlVariable(L"Sdl3_Dynamic_Api"));
    EXPECT_TRUE(isSdlVariable(L"SDL_"));

    EXPECT_FALSE(isSdlVariable(L""));
    EXPECT_FALSE(isSdlVariable(L"SDL"));
    EXPECT_FALSE(isSdlVariable(L"SDL3"));
    EXPECT_FALSE(isSdlVariable(L"SDLX"));
    EXPECT_FALSE(isSdlVariable(L"SDL3X_A"));
    EXPECT_FALSE(isSdlVariable(L"SD_L"));
    EXPECT_FALSE(isSdlVariable(L"MYSDL_THING"));
    EXPECT_FALSE(isSdlVariable(L"PATH"));
    EXPECT_FALSE(isSdlVariable(L"TASKSMACK_CONFIG_DIR"));
}

TEST(SdlEnvironmentTest, TheBlocksSdlNamesComeInOrderWithoutTheirValues)
{
    EXPECT_EQ(sdlVariableNames(BLOCK), sdlNames());
    EXPECT_TRUE(sdlVariableNames(block(L"\0\0")).empty());
    EXPECT_TRUE(sdlVariableNames(block(L"")).empty());
    // A per-drive entry is no variable, even one that looks like SDL's.
    EXPECT_TRUE(sdlVariableNames(block(L"=SDL_X=1\0\0")).empty());
    // A block with no empty string at its end is read to its end.
    EXPECT_EQ(sdlVariableNames(block(L"A=1\0SDL_X=2")), std::vector<std::wstring>{L"SDL_X"});
    // An entry with no '=' is skipped.
    EXPECT_TRUE(sdlVariableNames(block(L"SDL_X\0\0")).empty());
}

/// A fake environment, matching names as the OS does: case-insensitively, beyond ASCII too. Only U+017F (long s,
/// which upper-cases to 'S') is modelled past ASCII, the case the block's ASCII fold misses.
class FakeEnvironment
{
  public:
    explicit FakeEnvironment(std::wstring_view entries)
    {
        std::size_t position = 0;
        while (position < entries.size() && entries[position] != L'\0')
        {
            const std::size_t end = std::min(entries.find(L'\0', position), entries.size());
            const std::wstring_view entry = entries.substr(position, end - position);
            const std::size_t equals = entry.find(L'=', 1);
            m_Entries.emplace_back(entry.substr(0, equals), equals == std::wstring_view::npos ? L"" : entry.substr(equals + 1));
            position = end + 1;
        }
    }

    /// The entries as a block, as GetEnvironmentStringsW() gives them.
    [[nodiscard]] std::wstring block() const
    {
        std::wstring text;
        for (const auto& [name, value] : m_Entries)
        {
            text.append(name).append(L"=").append(value).push_back(L'\0');
        }
        text.push_back(L'\0');
        return text;
    }

    [[nodiscard]] std::vector<std::wstring> names() const
    {
        std::vector<std::wstring> result;
        result.reserve(m_Entries.size());
        for (const auto& [name, value] : m_Entries)
        {
            result.push_back(name);
        }
        return result;
    }

    /// The operations, which record each name asked about or removed and refuse
    /// to remove @p refused.
    [[nodiscard]] EnvironmentOps ops(std::wstring refused = {})
    {
        return EnvironmentOps{
            .isSet =
                [this](const std::wstring& name)
            {
                m_Asked.push_back(name);
                return std::ranges::any_of(m_Entries, [&name](const auto& entry) { return osEqual(entry.first, name); });
            },
            .remove =
                [this, refused = std::move(refused)](const std::wstring& name)
            {
                m_Removals.push_back(name);
                if (!refused.empty() && osEqual(name, refused))
                {
                    return false;
                }
                const auto found = std::ranges::find_if(m_Entries, [&name](const auto& entry) { return osEqual(entry.first, name); });
                if (found == m_Entries.end())
                {
                    return false;
                }
                m_Entries.erase(found);
                return true;
            },
        };
    }

    [[nodiscard]] const std::vector<std::wstring>& asked() const noexcept
    {
        return m_Asked;
    }

    [[nodiscard]] const std::vector<std::wstring>& removals() const noexcept
    {
        return m_Removals;
    }

  private:
    [[nodiscard]] static wchar_t osUpper(wchar_t c) noexcept
    {
        if (c == L'\u017F')
        {
            return L'S';
        }
        return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
    }

    [[nodiscard]] static bool osEqual(std::wstring_view a, std::wstring_view b) noexcept
    {
        return std::ranges::equal(a, b, [](wchar_t x, wchar_t y) { return osUpper(x) == osUpper(y); });
    }

    std::vector<std::pair<std::wstring, std::wstring>> m_Entries;
    std::vector<std::wstring> m_Asked;
    std::vector<std::wstring> m_Removals;
};

TEST(SdlEnvironmentTest, System32OnlyRemovesEverySdlVariable)
{
    FakeEnvironment environment(BLOCK);
    const std::wstring text = environment.block();
    const std::vector<std::wstring> removed = removeSdlVariables(true, text, environment.ops());
    EXPECT_EQ(removed, sdlNames());
    EXPECT_EQ(environment.names(), (std::vector<std::wstring>{L"=C:", L"PATH", L"SDLX", L"MYSDL_THING", L"TASKSMACK_CONFIG_DIR"}));
    // Every dangerous name is asked about by name as well; none is still set, so
    // none is removed twice.
    for (const std::wstring_view name : BY_NAME_VARIABLES)
    {
        EXPECT_NE(std::ranges::find(environment.asked(), std::wstring(name)), environment.asked().end()) << std::wstring(name);
    }
    EXPECT_EQ(environment.removals(), sdlNames());
}

TEST(SdlEnvironmentTest, OnlyTheVariablesActuallyRemovedAreReported)
{
    // SDL_VIDEODRIVER can't be removed: not reported from the block, nor by name,
    // where it is found but stays.
    FakeEnvironment environment(BLOCK);
    const std::wstring text = environment.block();
    std::vector<std::wstring> stillSet;
    const std::vector<std::wstring> removed = removeSdlVariables(true, text, environment.ops(L"SDL_VIDEODRIVER"), &stillSet);
    EXPECT_EQ(removed, (std::vector<std::wstring>{L"SDL_OPENGL_LIBRARY", L"sdl3_dynamic_api", L"Sdl_Egl_Library"}));
    // Failing closed (#4114): named once, though both passes found it.
    EXPECT_EQ(stillSet, std::vector<std::wstring>{L"SDL_VIDEODRIVER"});
}

TEST(SdlEnvironmentTest, NothingIsStillSetWhenEverythingWasRemoved)
{
    FakeEnvironment environment(BLOCK);
    const std::wstring text = environment.block();
    std::vector<std::wstring> stillSet;
    static_cast<void>(removeSdlVariables(true, text, environment.ops(), &stillSet));
    EXPECT_TRUE(stillSet.empty());
}

TEST(SdlEnvironmentTest, AByNameRemovalThatFailsLeavesTheNameStillSet)
{
    FakeEnvironment environment(block(L"SDL_OPENGL_LIBRARY=x.dll\0PATH=x\0\0"));
    std::vector<std::wstring> stillSet;
    const std::vector<std::wstring> removed = removeSdlVariables(true, std::nullopt, environment.ops(L"SDL_OPENGL_LIBRARY"), &stillSet);
    EXPECT_TRUE(removed.empty());
    EXPECT_EQ(stillSet, std::vector<std::wstring>{L"SDL_OPENGL_LIBRARY"});
}

TEST(SdlEnvironmentTest, EightEntriesOfOneNameAreRemovedAndNineAreFlagged)
{
    const auto entries = [](int count)
    {
        std::wstring text;
        for (int index = 0; index < count; ++index)
        {
            text += L"SDL3_DYNAMIC_API=x";
            text.push_back(L'\0');
        }
        text.push_back(L'\0');
        return text;
    };
    for (const int count : {8, 9})
    {
        FakeEnvironment environment(entries(count));
        std::vector<std::wstring> stillSet;
        const std::vector<std::wstring> removed = removeSdlVariables(true, std::nullopt, environment.ops(), &stillSet);
        EXPECT_EQ(removed, std::vector<std::wstring>{L"SDL3_DYNAMIC_API"}) << count;
        if (count == 8)
        {
            EXPECT_TRUE(stillSet.empty());
        }
        else
        {
            EXPECT_EQ(stillSet, std::vector<std::wstring>{L"SDL3_DYNAMIC_API"});
        }
    }
}

TEST(SdlEnvironmentTest, ABlockNameStillSetAfterARemovalThatSaidItWorkedIsFlagged)
{
    // remove() says true but took another entry (names that compare equal): asked again, it is still set.
    const EnvironmentOps wrongEntry{
        .isSet = [](const std::wstring& name) { return name == L"SDL_FOO"; },
        .remove = [](const std::wstring& /*name*/) { return true; },
    };
    std::vector<std::wstring> stillSet;
    static_cast<void>(removeSdlVariables(true, block(L"SDL_FOO=1\0\0"), wrongEntry, &stillSet));
    EXPECT_EQ(stillSet, std::vector<std::wstring>{L"SDL_FOO"});
}

TEST(SdlEnvironmentTest, AStillSetNameIsNotRecordedTwiceInAnotherCase)
{
    // The block's "sdl_videodriver" and the by-name "SDL_VIDEODRIVER" are one variable to the OS.
    const EnvironmentOps stuck{
        .isSet = [](const std::wstring& name) { return _wcsicmp(name.c_str(), L"SDL_VIDEODRIVER") == 0; },
        .remove = [](const std::wstring& /*name*/) { return false; },
    };
    std::vector<std::wstring> stillSet;
    static_cast<void>(removeSdlVariables(true, block(L"sdl_videodriver=x\0\0"), stuck, &stillSet));
    EXPECT_EQ(stillSet, std::vector<std::wstring>{L"sdl_videodriver"});
}

TEST(SdlEnvironmentTest, AByNameVariableThatRemovalDidNotClearIsStillSet)
{
    // With no block, so only the by-name pass: the OS says it removed it, and still answers that it is set.
    const EnvironmentOps stuck{
        .isSet = [](const std::wstring& name) { return name == L"SDL3_DYNAMIC_API"; },
        .remove = [](const std::wstring& /*name*/) { return true; },
    };
    std::vector<std::wstring> stillSet;
    static_cast<void>(removeSdlVariables(true, std::nullopt, stuck, &stillSet));
    EXPECT_EQ(stillSet, std::vector<std::wstring>{L"SDL3_DYNAMIC_API"});

    // Without the system32-only rule nothing is asked, so nothing is still set.
    stillSet.clear();
    static_cast<void>(removeSdlVariables(false, std::nullopt, stuck, &stillSet));
    EXPECT_TRUE(stillSet.empty());
}

TEST(SdlEnvironmentTest, ADangerousNameTheAsciiFoldMissesIsRemovedByName)
{
    // "ſDL3_DYNAMIC_API" (U+017F) isn't SDL's by the block's ASCII fold, but the
    // OS answers SDL3_DYNAMIC_API with it.
    FakeEnvironment environment(block(L"\u017FDL3_DYNAMIC_API=C:\\Users\\me\\SDL3.dll\0\u017Fdl_OPENGL_"
                                      L"LIBRARY=x.dll\0PATH=x\0\0"));
    const std::wstring text = environment.block();
    EXPECT_TRUE(sdlVariableNames(text).empty());
    const std::vector<std::wstring> removed = removeSdlVariables(true, text, environment.ops());
    EXPECT_EQ(removed, (std::vector<std::wstring>{L"SDL3_DYNAMIC_API", L"SDL_OPENGL_LIBRARY"}));
    EXPECT_EQ(environment.names(), std::vector<std::wstring>{L"PATH"});
}

TEST(SdlEnvironmentTest, WithNoBlockTheDangerousNamesAreStillRemovedByName)
{
    // GetEnvironmentStringsW() failed: it fails closed on the names that load a
    // library or pick a driver.
    FakeEnvironment environment(block(L"SDL_OPENGL_LIBRARY=x.dll\0sdl3_dynamic_"
                                      L"api=y.dll\0SDL_OTHER=1\0PATH=x\0\0"));
    const std::vector<std::wstring> removed = removeSdlVariables(true, std::nullopt, environment.ops());
    EXPECT_EQ(removed, (std::vector<std::wstring>{L"SDL3_DYNAMIC_API", L"SDL_OPENGL_LIBRARY"}));
    for (const std::wstring_view name : BY_NAME_VARIABLES)
    {
        EXPECT_NE(std::ranges::find(environment.asked(), name), environment.asked().end()) << std::wstring(name);
    }
    // Only the block pass finds the rest of SDL's variables.
    EXPECT_EQ(environment.names(), (std::vector<std::wstring>{L"SDL_OTHER", L"PATH"}));
}

TEST(SdlEnvironmentTest, ANameThatIsNotSetIsNeitherRemovedNorReported)
{
    FakeEnvironment environment(block(L"PATH=x\0\0"));
    EXPECT_TRUE(removeSdlVariables(true, std::nullopt, environment.ops()).empty());
    EXPECT_TRUE(environment.removals().empty());
}

TEST(SdlEnvironmentTest, TheByNamePassStopsOnAVariableThatStaysSet)
{
    // An environment that says each removal worked but keeps the variable: a
    // bounded number of tries, reported once.
    int removals = 0;
    const EnvironmentOps stuck{
        .isSet = [](const std::wstring& name) { return name == L"SDL3_DYNAMIC_API"; },
        .remove =
            [&removals](const std::wstring& /*name*/)
        {
            ++removals;
            return true;
        },
    };
    EXPECT_EQ(removeSdlVariables(true, std::nullopt, stuck), std::vector<std::wstring>{L"SDL3_DYNAMIC_API"});
    EXPECT_GT(removals, 1);
    EXPECT_LE(removals, 8);
}

TEST(SdlEnvironmentTest, OtherwiseNothingIsAskedOrRemoved)
{
    FakeEnvironment environment(BLOCK);
    const std::wstring text = environment.block();
    EXPECT_TRUE(removeSdlVariables(false, text, environment.ops()).empty());
    EXPECT_TRUE(removeSdlVariables(false, std::nullopt, environment.ops()).empty());
    EXPECT_TRUE(environment.asked().empty());
    EXPECT_TRUE(environment.removals().empty());
}

TEST(SdlEnvironmentTest, ALoggedNameCannotForgeALogLine)
{
    EXPECT_EQ(logName(L"SDL_OPENGL_LIBRARY"), "SDL_OPENGL_LIBRARY");
    EXPECT_EQ(logName(L"SDL_x\r\n[error] forged"), "SDL_x\\x0D\\x0A[error] forged");
    EXPECT_EQ(logName(L"SDL_\t\x1B[31m\x7F"), "SDL_\\x09\\x1B[31m\\x7F");
    EXPECT_EQ(logName(L"SDL_\x85\u2028\u2029"), "SDL_\\x85\\u2028\\u2029");
    EXPECT_EQ(logName(L"SDL_\u00E9"), "SDL_\xC3\xA9");
}

TEST(SdlEnvironmentTest, ABackslashInALoggedNameIsDoubledSoAnEscapeCannotBeForged)
{
    EXPECT_EQ(logName(L"SDL_a\\b"), "SDL_a\\\\b");
    // A literal backslash-x-0-D is not the escape of a carriage return, and the two read differently.
    EXPECT_EQ(logName(L"SDL_\\x0D"), "SDL_\\\\x0D");
    EXPECT_EQ(logName(L"SDL_\r"), "SDL_\\x0D");
    EXPECT_NE(logName(L"SDL_\\x0D"), logName(L"SDL_\r"));
    EXPECT_EQ(logName(L"SDL_\\u202E"), "SDL_\\\\u202E");
}

TEST(SdlEnvironmentTest, BidirectionalControlsInALoggedNameAreEscaped)
{
    for (const unsigned code : {0x061CU, 0x200EU, 0x200FU, 0x202AU, 0x202BU, 0x202CU, 0x202DU, 0x202EU, 0x2066U, 0x2067U, 0x2068U, 0x2069U})
    {
        const std::wstring name = std::wstring(L"SDL_a") + static_cast<wchar_t>(code) + L"b";
        EXPECT_EQ(logName(name), std::format("SDL_a\\u{:04X}b", code)) << std::hex << code;
    }
    // The characters either side of those ranges are not controls.
    for (const wchar_t outside : {L'\u061B', L'\u061D', L'\u200D', L'\u2010', L'\u202F', L'\u2065', L'\u206A'})
    {
        const std::wstring name = std::wstring(L"SDL_") + outside;
        EXPECT_EQ(logName(name), Platform::WinString::wideToUtf8(name)) << std::hex << static_cast<unsigned>(outside);
    }
}

TEST(SdlEnvironmentTest, AtMostThirtyTwoNamesAreLoggedThenAndNMore)
{
    std::vector<std::wstring> names;
    names.reserve(MAX_LOGGED_NAMES + 5);
    for (std::size_t index = 0; index < MAX_LOGGED_NAMES; ++index)
    {
        names.push_back(L"SDL_N" + std::to_wstring(index));
    }
    EXPECT_EQ(MAX_LOGGED_NAMES, 32U);
    EXPECT_EQ(loggedNames({}), std::vector<std::string>{});
    // Exactly the limit: all of them, no "and more".
    const std::vector<std::string> all = loggedNames(names);
    ASSERT_EQ(all.size(), MAX_LOGGED_NAMES);
    EXPECT_EQ(all.front(), "SDL_N0");
    EXPECT_EQ(all.back(), "SDL_N31");

    for (std::size_t index = 0; index < 5; ++index)
    {
        names.push_back(L"SDL_M" + std::to_wstring(index));
    }
    const std::vector<std::string> capped = loggedNames(names);
    ASSERT_EQ(capped.size(), MAX_LOGGED_NAMES + 1);
    EXPECT_EQ(capped[MAX_LOGGED_NAMES - 1], "SDL_N31");
    EXPECT_EQ(capped.back(), "and 5 more");
}

TEST(SdlEnvironmentTest, ALoggedNameIsCutAtItsLimitWithoutSplittingACharacter)
{
    const std::wstring longName = L"SDL_" + std::wstring(500, L'A');
    const std::string logged = logName(longName);
    EXPECT_EQ(logged, "SDL_" + std::string(MAX_LOGGED_NAME - 4, 'A') + "...");

    // A surrogate pair straddling the limit is dropped whole, not halved.
    std::wstring straddling = L"SDL_" + std::wstring(MAX_LOGGED_NAME - 5, L'A');
    straddling += L"\U0001F600";
    straddling += L"tail";
    EXPECT_EQ(logName(straddling), "SDL_" + std::string(MAX_LOGGED_NAME - 5, 'A') + "...");
    EXPECT_EQ(logName(std::wstring(MAX_LOGGED_NAME, L'B')), std::string(MAX_LOGGED_NAME, 'B'));
}

/// This process's environment: its SDL variables saved before each test and put back after, and three planted ones
/// (through the CRT, in this process only, each cleared in TearDown).
/// The planted names are SDL's form, "SDL_" and "SDL<digits>_" as SDL3_DYNAMIC_API is, one in mixed case, but no
/// variable SDL reads (no *_LIBRARY, *DRIVER or SDL3_DYNAMIC_API):
/// a test must never plant one that makes SDL load a library, since SDL's dynamic API reads SDL3_DYNAMIC_API at the
/// process's first SDL call and, when it can't load what that names, blocks on a message box. No SDL call is made
/// while anything is planted; SDL's view is checked only after the scrub, when the process has no SDL variable left.
class SdlEnvironmentProcessTest : public ::testing::Test
{
  protected:
    static constexpr const wchar_t* PLANTED = L"SDL_TASKSMACK_TEST_PLANTED";
    static constexpr const wchar_t* PLANTED_DIGITS = L"SDL9_TASKSMACK_TEST_PLANTED";
    static constexpr const wchar_t* PLANTED_MIXED_CASE = L"sdl_Tasksmack_Test_Planted_Lower";
    static constexpr const wchar_t* KEPT = L"TASKSMACK_SDL_ENVIRONMENT_TEST_KEPT";

    void SetUp() override
    {
        if (wchar_t* const strings = GetEnvironmentStringsW(); strings != nullptr)
        {
            for (const wchar_t* entry = strings; *entry != L'\0'; entry += std::wstring_view(entry).size() + 1)
            {
                const std::wstring_view text(entry);
                if (const std::size_t equals = text.find(L'=');
                    equals != std::wstring_view::npos && equals > 0 && isSdlVariable(text.substr(0, equals)))
                {
                    m_Saved.emplace_back(text.substr(0, equals), text.substr(equals + 1));
                }
            }
            FreeEnvironmentStringsW(strings);
        }
        // Through the CRT, which sets the block too (what SetEnvironmentVariableW alone would not do to the CRT's copy),
        // so the check on the CRT's copy after the scrub proves something (#4114). In this process only: nothing here
        // touches the registry, HKCU\Environment or a child's environment.
        ASSERT_EQ(_wputenv_s(PLANTED, L"C:\\Users\\Public\\planted.dll"), 0);
        ASSERT_EQ(_wputenv_s(PLANTED_DIGITS, L"C:\\Users\\Public\\planted.dll"), 0);
        ASSERT_EQ(_wputenv_s(PLANTED_MIXED_CASE, L"1"), 0);
        ASSERT_EQ(_wputenv_s(KEPT, L"1"), 0);
    }

    void TearDown() override
    {
        // Everything planted comes out of both copies, as it went in, whether or not the test's scrub already did it.
        for (const wchar_t* const name : {PLANTED, PLANTED_DIGITS, PLANTED_MIXED_CASE, KEPT})
        {
            static_cast<void>(_wputenv_s(name, L""));
            SetEnvironmentVariableW(name, nullptr);
        }
        // Through the CRT, which sets the block too: the scrub cleared the CRT's copy as well.
        for (const auto& [name, value] : m_Saved)
        {
            static_cast<void>(_wputenv_s(name.c_str(), value.c_str()));
        }
        // Clears what removedFromThisProcess() reports, as an unelevated TaskSmack's is.
        static_cast<void>(removeForThisProcess(false));
    }

    [[nodiscard]] static bool isSet(const wchar_t* name)
    {
        return GetEnvironmentVariableW(name, nullptr, 0) > 0;
    }

    /// Whether the C runtime's copy has @p name.
    [[nodiscard]] static bool crtHas(const wchar_t* name)
    {
        std::size_t size = 0;
        return _wgetenv_s(&size, nullptr, 0, name) == 0 && size > 0;
    }

  private:
    std::vector<std::pair<std::wstring, std::wstring>> m_Saved;
};

TEST_F(SdlEnvironmentProcessTest, System32OnlyRemovesEverySdlVariableBeforeSdlCanReadOne)
{
    // Planted in both copies, so the CRT checks below are about the scrub and not about what was never there.
    ASSERT_TRUE(isSet(PLANTED));
    ASSERT_TRUE(crtHas(PLANTED));
    ASSERT_TRUE(crtHas(PLANTED_DIGITS));
    ASSERT_TRUE(crtHas(PLANTED_MIXED_CASE));
    ASSERT_TRUE(crtHas(KEPT));

    const std::vector<std::wstring> removed = removeForThisProcess(true);
    EXPECT_TRUE(stillSetInThisProcess().empty());

    EXPECT_NE(std::ranges::find(removed, std::wstring(PLANTED)), removed.end());
    EXPECT_NE(std::ranges::find(removed, std::wstring(PLANTED_DIGITS)), removed.end());
    EXPECT_FALSE(isSet(PLANTED));
    EXPECT_FALSE(isSet(PLANTED_DIGITS));
    EXPECT_FALSE(isSet(PLANTED_MIXED_CASE));
    EXPECT_NE(std::ranges::find(removed, std::wstring(PLANTED_MIXED_CASE)), removed.end());
    EXPECT_FALSE(crtHas(PLANTED)); // The CRT's copy too
    EXPECT_FALSE(crtHas(PLANTED_DIGITS));
    EXPECT_FALSE(crtHas(PLANTED_MIXED_CASE));
    EXPECT_TRUE(isSet(KEPT));
    EXPECT_TRUE(crtHas(KEPT));

    // Nothing of SDL's is left, so SDL may now be called. A fresh SDL environment, built from the process's as
    // SDL_GetHint()'s fallback builds its own at first use (which may already exist in this test process), has no
    // SDL variable; and SDL_getenv_unsafe() is GetEnvironmentVariableA() on Windows, the read SDL's dynamic API makes.
    SDL_Environment* const environment = SDL_CreateEnvironment(true);
    ASSERT_NE(environment, nullptr) << SDL_GetError();
    EXPECT_EQ(SDL_GetEnvironmentVariable(environment, "SDL_TASKSMACK_TEST_PLANTED"), nullptr);
    EXPECT_EQ(SDL_GetEnvironmentVariable(environment, "SDL9_TASKSMACK_TEST_PLANTED"), nullptr);
    EXPECT_NE(SDL_GetEnvironmentVariable(environment, "TASKSMACK_SDL_ENVIRONMENT_TEST_KEPT"), nullptr);
    SDL_DestroyEnvironment(environment);
    EXPECT_EQ(SDL_getenv_unsafe("SDL_TASKSMACK_TEST_PLANTED"), nullptr);
    EXPECT_EQ(SDL_getenv_unsafe("SDL3_DYNAMIC_API"), nullptr);
    EXPECT_EQ(SDL_getenv_unsafe("SDL_OPENGL_LIBRARY"), nullptr);

    const std::vector<std::string>& logged = removedFromThisProcess();
    EXPECT_EQ(logged, loggedNames(removed));
    EXPECT_NE(std::ranges::find(logged, std::string("SDL_TASKSMACK_TEST_PLANTED")), logged.end());
    EXPECT_NE(std::ranges::find(logged, std::string("SDL9_TASKSMACK_TEST_PLANTED")), logged.end());
}

TEST_F(SdlEnvironmentProcessTest, OtherwiseTheyStay)
{
    // No SDL call here: the planted variables stay set throughout.
    EXPECT_TRUE(removeForThisProcess(false).empty());
    EXPECT_TRUE(removedFromThisProcess().empty());
    EXPECT_TRUE(isSet(PLANTED));
    EXPECT_TRUE(isSet(PLANTED_DIGITS));
    EXPECT_TRUE(isSet(PLANTED_MIXED_CASE));
    EXPECT_TRUE(isSet(KEPT));
    EXPECT_TRUE(crtHas(PLANTED));
    EXPECT_TRUE(stillSetInThisProcess().empty());
}

} // namespace
} // namespace Platform::Windows::SdlEnvironment
