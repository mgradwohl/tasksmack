/// @file test_WindowsStartupProbeMath.cpp
/// @brief WindowsStartupProbeMath.h (#801): the StartupApproved value (enabled/disabled byte, the
/// disable FILETIME, short and corrupt values), FILETIME to Unix time, scope, and the executable a Run
/// command line starts. No Windows header, so these run on every platform.

#include "Platform/IStartupProbe.h"
#include "Platform/Windows/WindowsStartupProbeMath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Platform::Windows::StartupMath
{
namespace
{

/// A 12-byte StartupApproved value: `flag`, three zero bytes, then `fileTime` little-endian.
[[nodiscard]] std::array<std::uint8_t, 12> approvedValue(std::uint8_t flag, std::uint64_t fileTime)
{
    std::array<std::uint8_t, 12> value{};
    value[0] = flag;
    for (std::size_t i = 0; i < 8; ++i)
    {
        value[4 + i] = static_cast<std::uint8_t>(fileTime >> (8U * i));
    }
    return value;
}

// 2024-01-02T03:04:05Z as a FILETIME, and as Unix seconds.
constexpr std::uint64_t UNIX_SECONDS = 1704164645;
constexpr std::uint64_t FILE_TIME = FILETIME_UNIX_EPOCH + (UNIX_SECONDS * FILETIME_TICKS_PER_SECOND);

[[nodiscard]] std::optional<ApprovedState> state(bool enabled, std::uint64_t disabledAt = 0)
{
    return ApprovedState{.enabled = enabled, .disabledAtUnixSeconds = disabledAt};
}

TEST(WindowsStartupProbeMathTest, EvenFirstByteIsEnabled)
{
    EXPECT_EQ(parseStartupApproved(approvedValue(0x02, 0)), state(true));
    EXPECT_EQ(parseStartupApproved(approvedValue(0x06, 0)), state(true));
}

TEST(WindowsStartupProbeMathTest, OddFirstByteIsDisabledWithItsTime)
{
    EXPECT_EQ(parseStartupApproved(approvedValue(0x03, FILE_TIME)), state(false, UNIX_SECONDS));
    EXPECT_EQ(parseStartupApproved(approvedValue(0x07, FILE_TIME)), state(false, UNIX_SECONDS));
}

TEST(WindowsStartupProbeMathTest, EnabledValueIgnoresAnyTime)
{
    EXPECT_EQ(parseStartupApproved(approvedValue(0x02, FILE_TIME)), state(true));
}

TEST(WindowsStartupProbeMathTest, ShortAndCorruptValues)
{
    EXPECT_EQ(parseStartupApproved({}), std::nullopt);

    // Too short for the FILETIME: the state still counts, with no time.
    constexpr std::array<std::uint8_t, 1> disabledOnly = {0x03};
    EXPECT_EQ(parseStartupApproved(disabledOnly), state(false));
    const auto truncated = approvedValue(0x03, FILE_TIME);
    EXPECT_EQ(parseStartupApproved(std::span(truncated).first(11)), state(false));

    // A disable time of zero, or one before 1970, is no time.
    EXPECT_EQ(parseStartupApproved(approvedValue(0x03, 0)), state(false));
    EXPECT_EQ(parseStartupApproved(approvedValue(0x03, FILETIME_UNIX_EPOCH - 1)), state(false));

    // An unexpected flag byte follows the parity rule; trailing bytes are ignored.
    std::array<std::uint8_t, 16> longer{};
    longer[0] = 0xFF;
    EXPECT_EQ(parseStartupApproved(longer), state(false));
}
TEST(WindowsStartupProbeMathTest, FileTimeToUnixSeconds)
{
    EXPECT_EQ(fileTimeToUnixSeconds(FILE_TIME), UNIX_SECONDS);
    EXPECT_EQ(fileTimeToUnixSeconds(FILETIME_UNIX_EPOCH), 0U);
    EXPECT_EQ(fileTimeToUnixSeconds(0), 0U);
}

TEST(WindowsStartupProbeMathTest, ScopeFollowsLocation)
{
    EXPECT_EQ(scopeOf(StartupLocation::RunUser), StartupScope::User);
    EXPECT_EQ(scopeOf(StartupLocation::RunOnceUser), StartupScope::User);
    EXPECT_EQ(scopeOf(StartupLocation::StartupFolderUser), StartupScope::User);
    EXPECT_EQ(scopeOf(StartupLocation::RunMachine), StartupScope::Machine);
    EXPECT_EQ(scopeOf(StartupLocation::RunMachine32), StartupScope::Machine);
    EXPECT_EQ(scopeOf(StartupLocation::RunOnceMachine), StartupScope::Machine);
    EXPECT_EQ(scopeOf(StartupLocation::StartupFolderCommon), StartupScope::Machine);
}

TEST(WindowsStartupProbeMathTest, ExecutableFromQuotedCommandLine)
{
    EXPECT_EQ(executableFromCommandLine(R"("C:\Program Files\App\app.exe" --minimized)"), R"(C:\Program Files\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine(R"(  "C:\Program Files\App\app.exe")"), R"(C:\Program Files\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine(R"("C:\Unterminated\app.exe)"), R"(C:\Unterminated\app.exe)");
}

TEST(WindowsStartupProbeMathTest, ExecutableFromUnquotedCommandLine)
{
    // Unquoted with spaces: up to the extension that ends a word.
    EXPECT_EQ(executableFromCommandLine(R"(C:\Program Files\App\app.exe /background)"), R"(C:\Program Files\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine(R"(C:\Program Files\App\APP.EXE)"), R"(C:\Program Files\App\APP.EXE)");
    EXPECT_EQ(executableFromCommandLine(R"(C:\Tools\sync.cmd -q)"), R"(C:\Tools\sync.cmd)");
    // ".exe" inside a word is not the end of the path.
    EXPECT_EQ(executableFromCommandLine(R"(C:\a.exefiles\run.exe -x)"), R"(C:\a.exefiles\run.exe)");
    // No known extension: the first token.
    EXPECT_EQ(executableFromCommandLine("rundll32 shell32.dll,Control_RunDLL"), "rundll32");
    EXPECT_EQ(executableFromCommandLine("ctfmon.exe"), "ctfmon.exe");
}

TEST(WindowsStartupProbeMathTest, ExecutableKeepsEnvironmentVariablesForTheProbe)
{
    EXPECT_EQ(executableFromCommandLine(R"(%ProgramFiles%\App\app.exe -x)"), R"(%ProgramFiles%\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine(R"("%LOCALAPPDATA%\App\app.exe" -x)"), R"(%LOCALAPPDATA%\App\app.exe)");
}

TEST(WindowsStartupProbeMathTest, TabsSeparateArgumentsLikeSpaces)
{
    // Windows command lines separate arguments with spaces or tabs.
    EXPECT_EQ(executableFromCommandLine("C:\\App\\app.exe\t--minimized"), R"(C:\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine("\"C:\\Program Files\\App\\app.exe\"\t--minimized"), R"(C:\Program Files\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine("\t \"C:\\App\\app.exe\""), R"(C:\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine("C:\\Program Files\\App\\app.exe\t/background"), R"(C:\Program Files\App\app.exe)");
    EXPECT_EQ(executableFromCommandLine("rundll32\tshell32.dll,Control_RunDLL"), "rundll32");
    EXPECT_EQ(executableFromCommandLine("\t\t"), "");
}

TEST(WindowsStartupProbeMathTest, EmptyCommandLineHasNoExecutable)
{
    EXPECT_EQ(executableFromCommandLine(""), "");
    EXPECT_EQ(executableFromCommandLine("   "), "");
    EXPECT_EQ(executableFromCommandLine(R"("")"), "");
}

} // namespace
} // namespace Platform::Windows::StartupMath
