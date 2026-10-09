/// @file test_SyscallTrace.cpp
/// @brief Tests for Platform::SyscallTrace (#182), the pure half of "Trace system calls (strace)":
/// PATH lookup, terminal selection from $TERMINAL and what is installed, the argv each terminal is
/// given, and the up-front ptrace check that turns a doomed attach into a clear message.

#include "Platform/Linux/ProcPrivileges.h"
#include "Platform/Linux/SyscallTrace.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::SyscallTrace
{
namespace
{

/// A fake filesystem: exactly these paths are executable.
struct FakeExecutables
{
    std::set<std::string> paths;

    [[nodiscard]] bool operator()(const std::string& path) const
    {
        return paths.contains(path);
    }
};

/// find() for selectTerminal(): findExecutable() over @p pathEnv in @p fs.
[[nodiscard]] auto finderOver(std::string_view pathEnv, const FakeExecutables& fs)
{
    return [pathEnv, &fs](std::string_view name)
    {
        return findExecutable(name, pathEnv, fs);
    };
}

// --- findExecutable --------------------------------------------------------------------------------

TEST(SyscallTraceTest, FindsTheFirstMatchInPathOrder)
{
    const FakeExecutables fs{.paths = {"/usr/bin/strace", "/usr/local/bin/strace"}};
    EXPECT_EQ(findExecutable("strace", "/usr/local/bin:/usr/bin", fs), "/usr/local/bin/strace");
    EXPECT_EQ(findExecutable("strace", "/usr/bin:/usr/local/bin", fs), "/usr/bin/strace");
    EXPECT_EQ(findExecutable("strace", "/usr/bin/", fs), "/usr/bin/strace");
    EXPECT_EQ(findExecutable("strace", "/bin:/sbin", fs), std::nullopt);
    EXPECT_EQ(findExecutable("strace", "", fs), std::nullopt);
    EXPECT_EQ(findExecutable("", "/usr/bin", fs), std::nullopt);
}

TEST(SyscallTraceTest, SkipsEmptyAndRelativePathEntries)
{
    // "." and "" (which POSIX reads as ".") would make what runs depend on the working directory.
    const FakeExecutables fs{.paths = {"./strace", "strace", "bin/strace", "/usr/bin/strace"}};
    EXPECT_EQ(findExecutable("strace", ":.:bin:/usr/bin", fs), "/usr/bin/strace");
    EXPECT_EQ(findExecutable("strace", ".:bin:", fs), std::nullopt);
}

TEST(SyscallTraceTest, ANameWithASlashIsUsedAsIsOnlyWhenAbsolute)
{
    const FakeExecutables fs{.paths = {"/opt/term/bin/myterm", "bin/myterm"}};
    EXPECT_EQ(findExecutable("/opt/term/bin/myterm", "/usr/bin", fs), "/opt/term/bin/myterm");
    EXPECT_EQ(findExecutable("/opt/term/bin/missing", "/usr/bin", fs), std::nullopt);
    EXPECT_EQ(findExecutable("bin/myterm", "/usr/bin", fs), std::nullopt);
}

// --- Terminal selection ----------------------------------------------------------------------------

TEST(SyscallTraceTest, TerminalEnvironmentVariableWinsWhenFound)
{
    const FakeExecutables fs{.paths = {"/usr/bin/kitty", "/usr/bin/x-terminal-emulator", "/usr/bin/xterm"}};
    const auto terminal = selectTerminal("kitty", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(*terminal, (Terminal{.path = "/usr/bin/kitty", .syntax = TerminalSyntax::Positional}));
}

TEST(SyscallTraceTest, TerminalEnvironmentVariableMayBeAnAbsolutePath)
{
    const FakeExecutables fs{.paths = {"/opt/bin/gnome-terminal", "/usr/bin/xterm"}};
    const auto terminal = selectTerminal("/opt/bin/gnome-terminal", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(*terminal, (Terminal{.path = "/opt/bin/gnome-terminal", .syntax = TerminalSyntax::DashDash}));
}

TEST(SyscallTraceTest, AnUnknownTerminalFromTheEnvironmentGetsDashE)
{
    const FakeExecutables fs{.paths = {"/usr/bin/myterm"}};
    const auto terminal = selectTerminal("myterm", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->syntax, TerminalSyntax::DashE);
}

TEST(SyscallTraceTest, TerminalEnvironmentVariableIsNeverSplitIntoWords)
{
    // "kitty -1" is not looked up as kitty with an option: no word splitting, no shell. With no such
    // file, selection falls back to the list.
    const FakeExecutables fs{.paths = {"/usr/bin/kitty", "/usr/bin/xterm"}};
    const auto terminal = selectTerminal("kitty -1", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->path, "/usr/bin/kitty"); // found by the fallback list, under its own name
}

TEST(SyscallTraceTest, FallsBackToXTerminalEmulatorFirst)
{
    const FakeExecutables fs{.paths = {"/usr/bin/x-terminal-emulator", "/usr/bin/gnome-terminal", "/usr/bin/xterm"}};
    const auto terminal = selectTerminal("", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(*terminal, (Terminal{.path = "/usr/bin/x-terminal-emulator", .syntax = TerminalSyntax::DashE}));
}

TEST(SyscallTraceTest, AnUnfoundTerminalVariableFallsBackToTheList)
{
    const FakeExecutables fs{.paths = {"/usr/bin/konsole", "/usr/bin/xterm"}};
    const auto terminal = selectTerminal("not-installed", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(*terminal, (Terminal{.path = "/usr/bin/konsole", .syntax = TerminalSyntax::DashE}));
}

TEST(SyscallTraceTest, ListOrderDecidesBetweenInstalledTerminals)
{
    const FakeExecutables fs{.paths = {"/usr/bin/xterm", "/usr/bin/xfce4-terminal"}};
    const auto terminal = selectTerminal("", finderOver("/usr/bin", fs));
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(*terminal, (Terminal{.path = "/usr/bin/xfce4-terminal", .syntax = TerminalSyntax::DashX}));
}

TEST(SyscallTraceTest, NoTerminalWhenNoneIsInstalled)
{
    const FakeExecutables fs{.paths = {"/usr/bin/strace"}};
    EXPECT_EQ(selectTerminal("", finderOver("/usr/bin", fs)), std::nullopt);
    EXPECT_EQ(selectTerminal("kitty", finderOver("/usr/bin", fs)), std::nullopt);
}

TEST(SyscallTraceTest, EachKnownTerminalHasItsOwnSyntax)
{
    EXPECT_EQ(terminalSyntaxFor("x-terminal-emulator"), TerminalSyntax::DashE);
    EXPECT_EQ(terminalSyntaxFor("gnome-terminal"), TerminalSyntax::DashDash);
    EXPECT_EQ(terminalSyntaxFor("/usr/bin/ptyxis"), TerminalSyntax::DashDash);
    EXPECT_EQ(terminalSyntaxFor("konsole"), TerminalSyntax::DashE);
    EXPECT_EQ(terminalSyntaxFor("xfce4-terminal"), TerminalSyntax::DashX);
    EXPECT_EQ(terminalSyntaxFor("mate-terminal"), TerminalSyntax::DashX);
    EXPECT_EQ(terminalSyntaxFor("kitty"), TerminalSyntax::Positional);
    EXPECT_EQ(terminalSyntaxFor("alacritty"), TerminalSyntax::DashE);
    EXPECT_EQ(terminalSyntaxFor("foot"), TerminalSyntax::Positional);
    EXPECT_EQ(terminalSyntaxFor("wezterm"), TerminalSyntax::WezTerm);
    EXPECT_EQ(terminalSyntaxFor("/usr/bin/xterm"), TerminalSyntax::DashE);
    EXPECT_EQ(terminalSyntaxFor("something-else"), TerminalSyntax::DashE);
}

// --- argv ------------------------------------------------------------------------------------------

TEST(SyscallTraceTest, TracerCommandAttachesToEveryThreadOfThePid)
{
    EXPECT_EQ(tracerCommand("/usr/bin/strace", 1234), (std::vector<std::string>{"/usr/bin/strace", "-f", "-tt", "-p", "1234"}));
}

TEST(SyscallTraceTest, BuildsEachTerminalsExecSyntax)
{
    const std::vector<std::string> command = tracerCommand("/usr/bin/strace", 77);
    const auto argvFor = [&command](std::string path, TerminalSyntax syntax)
    {
        return buildTerminalArgv(Terminal{.path = std::move(path), .syntax = syntax}, command);
    };

    EXPECT_EQ(argvFor("/usr/bin/xterm", TerminalSyntax::DashE),
              (std::vector<std::string>{"/usr/bin/xterm", "-e", "/usr/bin/strace", "-f", "-tt", "-p", "77"}));
    EXPECT_EQ(argvFor("/usr/bin/gnome-terminal", TerminalSyntax::DashDash),
              (std::vector<std::string>{"/usr/bin/gnome-terminal", "--", "/usr/bin/strace", "-f", "-tt", "-p", "77"}));
    EXPECT_EQ(argvFor("/usr/bin/xfce4-terminal", TerminalSyntax::DashX),
              (std::vector<std::string>{"/usr/bin/xfce4-terminal", "-x", "/usr/bin/strace", "-f", "-tt", "-p", "77"}));
    EXPECT_EQ(argvFor("/usr/bin/kitty", TerminalSyntax::Positional),
              (std::vector<std::string>{"/usr/bin/kitty", "/usr/bin/strace", "-f", "-tt", "-p", "77"}));
    EXPECT_EQ(argvFor("/usr/bin/wezterm", TerminalSyntax::WezTerm),
              (std::vector<std::string>{"/usr/bin/wezterm", "start", "--", "/usr/bin/strace", "-f", "-tt", "-p", "77"}));
}

TEST(SyscallTraceTest, ArgumentsArePassedThroughUntouched)
{
    // Every element goes to execve() as is: shell syntax stays literal text in one argument.
    const std::vector<std::string> command{"/usr/bin/strace", "-p", "1; rm -rf ~", "$(id)"};
    const auto argv = buildTerminalArgv(Terminal{.path = "/usr/bin/xterm", .syntax = TerminalSyntax::DashE}, command);
    EXPECT_EQ(argv, (std::vector<std::string>{"/usr/bin/xterm", "-e", "/usr/bin/strace", "-p", "1; rm -rf ~", "$(id)"}));
}

// --- /proc parsing ---------------------------------------------------------------------------------

TEST(SyscallTraceTest, ParsesPtraceScope)
{
    EXPECT_EQ(parsePtraceScope("0\n"), 0);
    EXPECT_EQ(parsePtraceScope("1\n"), 1);
    EXPECT_EQ(parsePtraceScope("3"), 3);
    EXPECT_EQ(parsePtraceScope("4\n"), std::nullopt);
    EXPECT_EQ(parsePtraceScope("-1\n"), std::nullopt);
    EXPECT_EQ(parsePtraceScope(""), std::nullopt);
    EXPECT_EQ(parsePtraceScope("x"), std::nullopt);
}

TEST(SyscallTraceTest, ParsesStatusIds)
{
    constexpr std::string_view STATUS = "Name:\tbash\nUid:\t1000\t1001\t1002\t1003\nGid:\t100\t100\t100\t100\n";
    EXPECT_EQ(parseStatusIds(STATUS, "Uid:"), (std::array<std::uint32_t, 3>{1000, 1001, 1002}));
    EXPECT_EQ(parseStatusIds(STATUS, "Gid:"), (std::array<std::uint32_t, 3>{100, 100, 100}));
    EXPECT_EQ(parseStatusIds(STATUS, "Groups:"), std::nullopt);
    EXPECT_EQ(parseStatusIds("Uid:\t1000\n", "Uid:"), std::nullopt);
    EXPECT_EQ(parseStatusIds("Uid:\tabc\t1\t1\t1\n", "Uid:"), std::nullopt);
}

TEST(SyscallTraceTest, ReadsCapSysPtraceFromFileCapabilities)
{
    // vfs_cap_data revision 2, effective flag set, permitted = 1 << 19 (CAP_SYS_PTRACE).
    constexpr std::array<std::uint8_t, 20> WITH_PTRACE{0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x08, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(fileCapsGrantSysPtrace(WITH_PTRACE));

    // Permitted but not effective: strace does not raise capabilities itself, so it would not have it.
    std::array<std::uint8_t, 20> notEffective = WITH_PTRACE;
    notEffective[0] = 0x00;
    EXPECT_FALSE(fileCapsGrantSysPtrace(notEffective));

    // Another capability (CAP_NET_RAW, bit 13) only.
    std::array<std::uint8_t, 20> netRaw = WITH_PTRACE;
    netRaw[5] = 0x20;
    netRaw[6] = 0x00;
    EXPECT_FALSE(fileCapsGrantSysPtrace(netRaw));

    EXPECT_FALSE(fileCapsGrantSysPtrace({}));
    EXPECT_FALSE(fileCapsGrantSysPtrace(std::span<const std::uint8_t>(WITH_PTRACE.data(), 6)));
}

// --- ptrace pre-check ------------------------------------------------------------------------------

TEST(SyscallTraceTest, RootTracerHasCapSysPtraceOnlyWhenCapEffSaysSo)
{
    constexpr std::uint64_t PTRACE_BIT = std::uint64_t{1} << ProcPrivileges::CAP_SYS_PTRACE_BIT;
    EXPECT_TRUE(rootTracerHasSysPtrace(true, PTRACE_BIT));
    EXPECT_TRUE(rootTracerHasSysPtrace(true, 0x000001ffffffffffULL));
    // Root with the capability dropped (a container, a hardened service).
    EXPECT_FALSE(rootTracerHasSysPtrace(true, 0x000001ffffffffffULL & ~PTRACE_BIT));
    EXPECT_FALSE(rootTracerHasSysPtrace(true, 0));
    // A non-root process's effective set is not kept across exec.
    EXPECT_FALSE(rootTracerHasSysPtrace(false, PTRACE_BIT));
}

TEST(SyscallTraceTest, UnreadableOrMalformedCapEffFailsClosed)
{
    // /proc/self/status unreadable: no CapEff at all.
    EXPECT_FALSE(rootTracerHasSysPtrace(true, std::nullopt));
    // Present but malformed, or missing from the status text.
    for (const std::string_view status : {"Name:\tx\nCapEff:\tzzzz\n", "Name:\tx\nCapEff:\t\n", "Name:\tx\nCapPrm:\t000001ffffffffff\n"})
    {
        SCOPED_TRACE(status);
        EXPECT_FALSE(rootTracerHasSysPtrace(true, ProcPrivileges::parseCapEff(status)));
    }
    EXPECT_TRUE(rootTracerHasSysPtrace(true, ProcPrivileges::parseCapEff("CapEff:\t0000000000080000\n")));
}

TEST(SyscallTraceTest, OwnProcessIsAllowedWithoutYamaOrInClassicMode)
{
    EXPECT_EQ(ptraceRefusal(42, {.ptraceScope = std::nullopt, .sameCredentials = true, .privileged = false}), std::nullopt);
    EXPECT_EQ(ptraceRefusal(42, {.ptraceScope = 0, .sameCredentials = true, .privileged = false}), std::nullopt);
}

TEST(SyscallTraceTest, AnotherUsersProcessNeedsCapSysPtrace)
{
    const auto refusal = ptraceRefusal(42, {.ptraceScope = 0, .sameCredentials = false, .privileged = false});
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("another user"), std::string::npos);
    EXPECT_NE(refusal->find("CAP_SYS_PTRACE"), std::string::npos);
    EXPECT_NE(refusal->find("42"), std::string::npos);
    EXPECT_EQ(ptraceRefusal(42, {.ptraceScope = 0, .sameCredentials = false, .privileged = true}), std::nullopt);
}

TEST(SyscallTraceTest, RestrictedYamaModesExplainThePtraceScopeSetting)
{
    for (const int scope : {1, 2})
    {
        SCOPED_TRACE(scope);
        const auto refusal = ptraceRefusal(42, {.ptraceScope = scope, .sameCredentials = true, .privileged = false});
        ASSERT_TRUE(refusal.has_value());
        EXPECT_NE(refusal->find("/proc/sys/kernel/yama/ptrace_scope is " + std::to_string(scope)), std::string::npos);
        EXPECT_NE(refusal->find("CAP_SYS_PTRACE"), std::string::npos);
        // With the capability, Yama modes 1 and 2 allow the attach.
        EXPECT_EQ(ptraceRefusal(42, {.ptraceScope = scope, .sameCredentials = true, .privileged = true}), std::nullopt);
    }
}

TEST(SyscallTraceTest, YamaModeThreeRefusesEvenRoot)
{
    const auto refusal = ptraceRefusal(42, {.ptraceScope = 3, .sameCredentials = true, .privileged = true});
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("/proc/sys/kernel/yama/ptrace_scope is 3"), std::string::npos);
}

// The Realtime I/O class needs CAP_SYS_NICE or CAP_SYS_ADMIN in TaskSmack's own effective set; unknown
// is no, so the I/O slider leaves Realtime out rather than offer a class each attempt would refuse (#1540).
TEST(SyscallTraceTest, RealtimeIoNeedsSysNiceOrSysAdmin)
{
    EXPECT_TRUE(ProcPrivileges::canSetRealtimeIoPriority(ProcPrivileges::parseCapEff("CapEff:\t0000000000800000\n")));  // bit 23
    EXPECT_TRUE(ProcPrivileges::canSetRealtimeIoPriority(ProcPrivileges::parseCapEff("CapEff:\t0000000000200000\n")));  // bit 21
    EXPECT_TRUE(ProcPrivileges::canSetRealtimeIoPriority(ProcPrivileges::parseCapEff("CapEff:\t000001ffffffffff\n")));  // full set
    EXPECT_FALSE(ProcPrivileges::canSetRealtimeIoPriority(ProcPrivileges::parseCapEff("CapEff:\t0000000000080000\n"))); // ptrace only
    EXPECT_FALSE(ProcPrivileges::canSetRealtimeIoPriority(ProcPrivileges::parseCapEff("CapEff:\t0000000000000000\n")));
    EXPECT_FALSE(ProcPrivileges::canSetRealtimeIoPriority(std::nullopt)); // unreadable
}

} // namespace
} // namespace Platform::SyscallTrace
