// Tests for DllSearchPolicy (#3897): whether TaskSmack.exe lets a DLL loaded by name come from System32 only, or
// from the folder it runs from as well, for each token elevation type, with and without Restart as administrator's
// --handoff. Pure functions of their inputs: nothing here changes this process's DLL search.

#include "Platform/Factory.h"
#include "Platform/Windows/DllSearchPolicy.h"
#include "Platform/Windows/WindowsElevation.h"

#include <gtest/gtest.h>

#include <string_view>

namespace Platform::Windows::DllSearchPolicy
{
namespace
{

constexpr std::wstring_view PLAIN = LR"("C:\Users\me\Downloads\TaskSmack\TaskSmack.exe")";
constexpr std::wstring_view MINIMIZED = LR"("C:\Users\me\Downloads\TaskSmack\TaskSmack.exe" --minimized)";
// As ElevatedRestart::parameters() writes it, after the program ShellExecuteExW puts first
constexpr std::wstring_view HANDOFF =
    LR"("C:\Users\me\Downloads\TaskSmack\TaskSmack.exe" --handoff 4242 133000000000000000 --config-dir "C:\Users\me\AppData\Roaming\TaskSmack")";

TEST(DllSearchPolicyTest, ElevatedThroughUacIsSystem32Only)
{
    EXPECT_TRUE(system32Only(TokenElevationTypeFull, PLAIN));
    EXPECT_TRUE(system32Only(TokenElevationTypeFull, MINIMIZED));
    EXPECT_TRUE(system32Only(TokenElevationTypeFull, HANDOFF));
}

TEST(DllSearchPolicyTest, ARestartAsAdministratorIsSystem32OnlyWhateverItsTokenType)
{
    // Over the shoulder to the built-in Administrator (RID 500), the elevated copy's token is Default, not Full.
    EXPECT_TRUE(system32Only(TokenElevationTypeDefault, HANDOFF));
    EXPECT_TRUE(system32Only(TokenElevationTypeLimited, HANDOFF));
}

TEST(DllSearchPolicyTest, ADefaultTokenWithoutTheHandOffKeepsItsOwnFolder)
{
    // A standard user, an administrator with no split token, a hosted CI runner whose tests run elevated with a
    // Default token: Mesa's opengl32.dll beside TaskSmack.exe must still load there.
    EXPECT_FALSE(system32Only(TokenElevationTypeDefault, PLAIN));
    EXPECT_FALSE(system32Only(TokenElevationTypeDefault, MINIMIZED));
    EXPECT_FALSE(system32Only(TokenElevationTypeDefault, L""));
}

TEST(DllSearchPolicyTest, ALimitedTokenKeepsItsOwnFolder)
{
    EXPECT_FALSE(system32Only(TokenElevationTypeLimited, PLAIN));
    EXPECT_FALSE(system32Only(TokenElevationTypeLimited, MINIMIZED));
}

TEST(DllSearchPolicyTest, TheHandOffIsFoundOnlyAsAnArgumentOfItsOwn)
{
    EXPECT_TRUE(hasHandoffArgument(HANDOFF));
    EXPECT_TRUE(hasHandoffArgument(L"TaskSmack.exe\t--handoff\t1 2"));
    EXPECT_TRUE(hasHandoffArgument(L"TaskSmack.exe \"--handoff\" 1 2"));
    EXPECT_TRUE(hasHandoffArgument(L"--handoff"));

    EXPECT_FALSE(hasHandoffArgument(PLAIN));
    EXPECT_FALSE(hasHandoffArgument(MINIMIZED));
    EXPECT_FALSE(hasHandoffArgument(L""));
    EXPECT_FALSE(hasHandoffArgument(L"TaskSmack.exe --handoffs 1 2"));
    EXPECT_FALSE(hasHandoffArgument(L"TaskSmack.exe -handoff 1 2"));
    EXPECT_FALSE(hasHandoffArgument(L"TaskSmack.exe --HANDOFF 1 2"));
    EXPECT_FALSE(hasHandoffArgument(L"TaskSmack.exe x--handoff"));
}

TEST(DllSearchPolicyTest, ThisProcessIsSystem32OnlyExactlyWhenElevatedThroughUac)
{
    // The test runner is never started with --handoff, so only its token decides.
    EXPECT_EQ(system32OnlyForThisProcess(), Platform::elevatedThroughUac());
}

TEST(DllSearchPolicyTest, ThisProcessReadsItsTokenThroughCurrentElevationType)
{
    // #4093: the token is read in one place, WindowsElevation's, even this early in WinMain.
    EXPECT_EQ(system32OnlyForThisProcess(), system32Only(currentElevationType(), GetCommandLineW()));
}

} // namespace
} // namespace Platform::Windows::DllSearchPolicy
