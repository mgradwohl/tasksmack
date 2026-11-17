// Tests for SdlEnvironment (#4103): the SDL variables an elevated TaskSmack.exe removes from its environment before
// any SDL call, so SDL loads no DLL a hint from the less-privileged side names (SDL_OPENGL_LIBRARY, SDL3_DYNAMIC_API).
// Through a fake environment block for every case, then on this process's own: only its environment changes, and
// each test puts back every SDL variable it found.

#include "Platform/Windows/SdlEnvironment.h"

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

TEST(SdlEnvironmentTest, System32OnlyRemovesEverySdlVariable)
{
    std::vector<std::wstring> asked;
    const std::vector<std::wstring> removed = removeSdlVariables(true,
                                                                 BLOCK,
                                                                 [&asked](const std::wstring& name)
                                                                 {
                                                                     asked.push_back(name);
                                                                     return true;
                                                                 });
    EXPECT_EQ(asked, sdlNames());
    EXPECT_EQ(removed, sdlNames());
}

TEST(SdlEnvironmentTest, OnlyTheVariablesActuallyRemovedAreReported)
{
    const std::vector<std::wstring> removed =
        removeSdlVariables(true, BLOCK, [](const std::wstring& name) { return name != L"SDL_VIDEODRIVER"; });
    EXPECT_EQ(removed, (std::vector<std::wstring>{L"SDL_OPENGL_LIBRARY", L"sdl3_dynamic_api", L"Sdl_Egl_Library"}));
}

TEST(SdlEnvironmentTest, OtherwiseNothingIsRemoved)
{
    bool called = false;
    const std::vector<std::wstring> removed = removeSdlVariables(false,
                                                                 BLOCK,
                                                                 [&called](const std::wstring& /*name*/)
                                                                 {
                                                                     called = true;
                                                                     return true;
                                                                 });
    EXPECT_FALSE(called);
    EXPECT_TRUE(removed.empty());
}

/// This process's environment: its SDL variables saved before each test and put back after, and two planted ones.
/// The planted names are SDL's form, "SDL_" and "SDL<digits>_" as SDL3_DYNAMIC_API is, but no variable SDL reads:
/// a test must never plant one that makes SDL load a library, since SDL's dynamic API reads SDL3_DYNAMIC_API at the
/// process's first SDL call and, when it can't load what that names, blocks on a message box. No SDL call is made
/// while anything is planted; SDL's view is checked only after the scrub, when the process has no SDL variable left.
class SdlEnvironmentProcessTest : public ::testing::Test
{
  protected:
    static constexpr const wchar_t* PLANTED = L"SDL_TASKSMACK_TEST_PLANTED";
    static constexpr const wchar_t* PLANTED_DIGITS = L"SDL9_TASKSMACK_TEST_PLANTED";
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
        ASSERT_NE(SetEnvironmentVariableW(PLANTED, L"C:\\Users\\Public\\planted.dll"), FALSE);
        ASSERT_NE(SetEnvironmentVariableW(PLANTED_DIGITS, L"C:\\Users\\Public\\planted.dll"), FALSE);
        ASSERT_NE(SetEnvironmentVariableW(KEPT, L"1"), FALSE);
    }

    void TearDown() override
    {
        SetEnvironmentVariableW(PLANTED, nullptr);
        SetEnvironmentVariableW(PLANTED_DIGITS, nullptr);
        SetEnvironmentVariableW(KEPT, nullptr);
        for (const auto& [name, value] : m_Saved)
        {
            SetEnvironmentVariableW(name.c_str(), value.c_str());
        }
        // Clears what removedFromThisProcess() reports, as an unelevated TaskSmack's is.
        static_cast<void>(removeForThisProcess(false));
    }

    [[nodiscard]] static bool isSet(const wchar_t* name)
    {
        return GetEnvironmentVariableW(name, nullptr, 0) > 0;
    }

  private:
    std::vector<std::pair<std::wstring, std::wstring>> m_Saved;
};

TEST_F(SdlEnvironmentProcessTest, System32OnlyRemovesEverySdlVariableBeforeSdlCanReadOne)
{
    const std::vector<std::wstring> removed = removeForThisProcess(true);

    EXPECT_NE(std::ranges::find(removed, std::wstring(PLANTED)), removed.end());
    EXPECT_NE(std::ranges::find(removed, std::wstring(PLANTED_DIGITS)), removed.end());
    EXPECT_FALSE(isSet(PLANTED));
    EXPECT_FALSE(isSet(PLANTED_DIGITS));
    EXPECT_TRUE(isSet(KEPT));

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
    EXPECT_EQ(logged.size(), removed.size());
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
    EXPECT_TRUE(isSet(KEPT));
}

} // namespace
} // namespace Platform::Windows::SdlEnvironment
