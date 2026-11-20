/// @file test_SyscallTrace.cpp
/// @brief Tests for Platform::SyscallTrace (#182), the pure half of "Trace system calls (strace)":
/// PATH lookup, terminal selection from $TERMINAL and what is installed, the argv each terminal is
/// given, and the up-front ptrace check that turns a doomed attach into a clear message.

#include "Platform/Linux/ProcPrivileges.h"
#include "Platform/Linux/RootTools.h"
#include "Platform/Linux/SyscallTrace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>

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

// --- onlyRootCouldPlace (#3605) ----------------------------------------------------------------------

/// A fake stat(): each listed path's owner and mode; anything else doesn't exist.
[[nodiscard]] auto ownershipOver(const std::map<std::string, PathOwnership>& entries)
{
    return [&entries](const std::string& path) -> std::optional<PathOwnership>
    {
        const auto it = entries.find(path);
        return it != entries.end() ? std::optional<PathOwnership>(it->second) : std::nullopt;
    };
}

constexpr std::uint32_t ROOT_DIR = 0040755;
constexpr std::uint32_t ROOT_TOOL = 0100755;

TEST(SyscallTraceTest, ARootOwnedToolInRootOwnedDirectoriesIsOneOnlyRootCouldPlace)
{
    const std::map<std::string, PathOwnership> fs{
        {"/", {.uid = 0, .mode = ROOT_DIR}},
        {"/usr", {.uid = 0, .mode = ROOT_DIR}},
        {"/usr/bin", {.uid = 0, .mode = ROOT_DIR}},
        {"/usr/bin/strace", {.uid = 0, .mode = ROOT_TOOL}},
    };
    EXPECT_TRUE(onlyRootCouldPlace("/usr/bin/strace", ownershipOver(fs)));
}

TEST(SyscallTraceTest, AToolAUserOwnsOrCouldReplaceIsNot)
{
    std::map<std::string, PathOwnership> fs{
        {"/", {.uid = 0, .mode = ROOT_DIR}},
        {"/home", {.uid = 0, .mode = ROOT_DIR}},
        {"/home/someone", {.uid = 1000, .mode = 0040755}},
        {"/home/someone/bin", {.uid = 1000, .mode = 0040755}},
        {"/home/someone/bin/strace", {.uid = 0, .mode = ROOT_TOOL}},
        {"/tmp", {.uid = 0, .mode = 0041777}},
        {"/tmp/gcore", {.uid = 0, .mode = ROOT_TOOL}},
        {"/usr", {.uid = 0, .mode = ROOT_DIR}},
        {"/usr/local", {.uid = 0, .mode = 0042775}},
        {"/usr/local/bin", {.uid = 0, .mode = ROOT_DIR}},
        {"/usr/local/bin/xterm", {.uid = 0, .mode = ROOT_TOOL}},
        {"/usr/bin", {.uid = 0, .mode = ROOT_DIR}},
        {"/usr/bin/mine", {.uid = 1000, .mode = ROOT_TOOL}},
        {"/usr/bin/shared", {.uid = 0, .mode = 0100775}},
    };
    const auto ownership = ownershipOver(fs);
    EXPECT_FALSE(onlyRootCouldPlace("/home/someone/bin/strace", ownership)) << "a directory above it is the user's";
    EXPECT_FALSE(onlyRootCouldPlace("/tmp/gcore", ownership)) << "anyone can write /tmp";
    EXPECT_FALSE(onlyRootCouldPlace("/usr/local/bin/xterm", ownership)) << "a group can write /usr/local";
    EXPECT_FALSE(onlyRootCouldPlace("/usr/bin/mine", ownership)) << "the file is a user's";
    EXPECT_FALSE(onlyRootCouldPlace("/usr/bin/shared", ownership)) << "a group can write the file";
    EXPECT_FALSE(onlyRootCouldPlace("/usr/bin/missing", ownership));
    EXPECT_FALSE(onlyRootCouldPlace("usr/bin/strace", ownership)) << "not absolute";
}

// The same check over this machine's filesystem, as TaskSmack makes it as root: the shell the system installed is one
// only root could place, and a file this test writes to the temp directory isn't.
TEST(SyscallTraceTest, OnThisMachineTheSystemShellPassesAndATempFileDoesNot)
{
    const auto realOwnership = [](const std::string& path) -> std::optional<PathOwnership>
    {
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0)
        {
            return std::nullopt;
        }
        return PathOwnership{.uid = info.st_uid, .mode = info.st_mode};
    };
    std::error_code ec;
    const std::filesystem::path shell = std::filesystem::canonical("/bin/sh", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(onlyRootCouldPlace(shell.native(), realOwnership)) << shell;

    const std::filesystem::path temp = std::filesystem::temp_directory_path() / "tasksmack-3605-tool";
    {
        std::ofstream(temp) << "#!/bin/sh\n";
    }
    const std::filesystem::path resolved = std::filesystem::canonical(temp, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_FALSE(onlyRootCouldPlace(resolved.native(), realOwnership)) << resolved;
    std::filesystem::remove(temp, ec);
}

TEST(SyscallTraceTest, AsRootToolsAreLookedUpOnlyInTheSystemsOwnDirectories)
{
    const FakeExecutables fs{{"/home/someone/bin/strace", "/usr/bin/strace", "/usr/local/sbin/gcore"}};
    EXPECT_EQ(findExecutable("strace", ROOT_TOOL_PATH, fs), std::optional<std::string>("/usr/bin/strace"));
    EXPECT_EQ(findExecutable("gcore", ROOT_TOOL_PATH, fs), std::optional<std::string>("/usr/local/sbin/gcore"));
    for (const std::string_view dir : {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin"})
    {
        EXPECT_TRUE(ROOT_TOOL_PATH.contains(dir)) << dir;
    }
    EXPECT_FALSE(ROOT_TOOL_PATH.contains("home"));
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
    EXPECT_EQ(parseStatusIds(STATUS, "Uid"), (std::array<std::uint32_t, 3>{1000, 1001, 1002}));
    EXPECT_EQ(parseStatusIds(STATUS, "Gid"), (std::array<std::uint32_t, 3>{100, 100, 100}));
    EXPECT_EQ(parseStatusIds(STATUS, "Groups"), std::nullopt);
    EXPECT_EQ(parseStatusIds("Uid:\t1000\n", "Uid"), std::nullopt);
    EXPECT_EQ(parseStatusIds("Uid:\tabc\t1\t1\t1\n", "Uid"), std::nullopt);
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

// #2140: which terminals hand their command to a server, and when that matters.
TEST(SyscallTraceTest, ATerminalServerCantCarryTaskSmacksRootToStrace)
{
    using SyscallTrace::handsCommandToServer;
    using SyscallTrace::serverTerminalRefusal;
    EXPECT_TRUE(handsCommandToServer("/usr/bin/gnome-terminal.wrapper"));
    EXPECT_TRUE(handsCommandToServer("/usr/bin/gnome-terminal"));
    EXPECT_TRUE(handsCommandToServer("/usr/bin/ptyxis"));
    EXPECT_TRUE(handsCommandToServer("kgx"));
    EXPECT_FALSE(handsCommandToServer("/usr/bin/xterm"));
    EXPECT_FALSE(handsCommandToServer("/usr/bin/konsole"));

    const SyscallTrace::PtraceContext rootOnly{
        .ptraceScope = 0, .sameCredentials = false, .privileged = true, .privilegeIsInherited = true};
    EXPECT_TRUE(serverTerminalRefusal(42, rootOnly, "/usr/bin/gnome-terminal.wrapper").has_value());
    EXPECT_FALSE(serverTerminalRefusal(42, rootOnly, "/usr/bin/xterm").has_value());
    // Yama's descendants-only mode needs the privilege even for one's own processes.
    const SyscallTrace::PtraceContext rootYama{.ptraceScope = 1, .sameCredentials = true, .privileged = true, .privilegeIsInherited = true};
    EXPECT_TRUE(serverTerminalRefusal(42, rootYama, "/usr/bin/ptyxis").has_value());
    // Not needed: the target is ours and Yama allows it.
    const SyscallTrace::PtraceContext own{.ptraceScope = 0, .sameCredentials = true, .privileged = true, .privilegeIsInherited = true};
    EXPECT_FALSE(serverTerminalRefusal(42, own, "/usr/bin/ptyxis").has_value());
    // strace's own file capability goes with it, whoever starts it.
    const SyscallTrace::PtraceContext fileCap{
        .ptraceScope = 1, .sameCredentials = false, .privileged = true, .privilegeIsInherited = false};
    EXPECT_FALSE(serverTerminalRefusal(42, fileCap, "/usr/bin/ptyxis").has_value());
}

// #4396: a link to a root-owned tool passed the check on its target, though the link itself sat in a folder another
// account could write, and could be pointed elsewhere before the exec. Now such a link is refused, and as root the path
// run is the one checked: since #4531, the path found, every link on whose way only root could change.
TEST(RootToolsTest, AsRootThePathCheckedIsThePathRun)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tasksmack-4396-link";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::filesystem::path link = dir / "sh";
    std::filesystem::create_symlink("/bin/sh", link, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::filesystem::path target = std::filesystem::canonical("/bin/sh", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_FALSE(RootTools::rootTrustedPath(link.string()).has_value())
        << "its target is root's, but the link sits in a folder this account can write, so it could choose or re-point it";
    EXPECT_EQ(RootTools::rootTrustedPath(target.string()), std::optional<std::string>(target.string()));

    // #4531: the path found is what runs, in a trusted folder (each one canonical, #4550), so a multi-call program
    // keeps the name it dispatches on: /usr/bin/sh, not the dash or bash it links to.
    const std::optional<std::string> found = RootTools::find("sh", true, "");
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->ends_with("/sh")) << *found;
    EXPECT_EQ(std::filesystem::path(*found).parent_path().string(),
              std::filesystem::canonical(std::filesystem::path(*found).parent_path(), ec).string())
        << "in a trusted folder, named canonically";
    std::filesystem::remove_all(dir, ec);
}

/// A fake tree for the #4396 checks: each entry's owner and mode, and the links. /usr/local and /usr/local/bin are
/// root:staff 2775, as Debian ships them (or as a chown -R of /usr/local leaves them).
struct FakeTree
{
    std::map<std::string, SyscallTrace::PathOwnership> entries;
    std::map<std::string, std::string> links; // absolute path -> absolute target

    /// canonical(): each component in turn, following a link wherever one is met, a folder's included (#4551 review).
    [[nodiscard]] std::optional<std::string> resolve(const std::string& path, int depth) const
    {
        if (depth > 40)
        {
            return std::nullopt;
        }
        std::string at;
        std::size_t start = 1;
        while (start <= path.size())
        {
            std::size_t end = path.find('/', start);
            if (end == std::string::npos)
            {
                end = path.size();
            }
            if (end > start)
            {
                at += "/" + path.substr(start, end - start);
                if (const auto link = links.find(at); link != links.end())
                {
                    const std::optional<std::string> target = resolve(link->second, depth + 1);
                    if (!target.has_value())
                    {
                        return std::nullopt;
                    }
                    at = *target == "/" ? std::string() : *target;
                }
                else if (!entries.contains(at))
                {
                    return std::nullopt;
                }
            }
            start = end + 1;
        }
        return at.empty() ? std::optional<std::string>("/") : std::optional(at);
    }

    [[nodiscard]] RootTools::RootFs fs() const
    {
        return {
            .ownership = [this](const std::string& path) -> std::optional<SyscallTrace::PathOwnership>
            {
                const auto found = entries.find(path);
                return found == entries.end() ? std::nullopt : std::optional(found->second);
            },
            .readLink = [this](const std::string& path) -> std::optional<std::string>
            {
                const auto found = links.find(path);
                return found == links.end() ? std::nullopt : std::optional(found->second);
            },
            .canonical = [this](const std::string& path) { return resolve(path, 0); },
            .executable =
                [this](const std::string& path)
            {
                const std::optional<std::string> resolved = fs().canonical(path);
                return resolved.has_value() && (entries.at(*resolved).mode & 0111U) != 0 && (entries.at(*resolved).mode & 0040000U) == 0;
            },
        };
    }
};

[[nodiscard]] FakeTree staffWritableUsrLocal()
{
    constexpr std::uint32_t DIR = 0040000;
    FakeTree tree;
    tree.entries = {
        {"/", {.uid = 0, .mode = DIR | 0755}},
        {"/usr", {.uid = 0, .mode = DIR | 0755}},
        {"/usr/local", {.uid = 0, .mode = DIR | 02775}},
        {"/usr/local/bin", {.uid = 0, .mode = DIR | 02775}},
        {"/usr/bin", {.uid = 0, .mode = DIR | 0755}},
        {"/usr/bin/python3", {.uid = 0, .mode = 0100755}},
        {"/usr/bin/xdg-open", {.uid = 0, .mode = 0100755}},
        {"/usr/bin/crontab", {.uid = 0, .mode = 0102755}},
    };
    tree.links = {
        {"/usr/local/bin/xdg-open", "/usr/bin/python3"}, // planted by an account in the staff group
        {"/usr/local/bin/crontab", "/usr/bin/crontab"},
        {"/usr/bin/hop", "/usr/local/bin/xdg-open"}, // a root-only link whose next hop isn't
    };
    tree.entries["/usr/local/bin/xdg-open"] = {.uid = 1000, .mode = 0120777};
    tree.entries["/usr/local/bin/crontab"] = {.uid = 1000, .mode = 0120777};
    tree.entries["/usr/bin/hop"] = {.uid = 0, .mode = 0120777};
    return tree;
}

// #4396, the security review: a staff-writable /usr/local/bin is left out of the root search and of a root child's
// PATH, so an account there chooses neither which program runs nor a script's helpers.
TEST(RootToolsTest, AFolderAnotherAccountCanWriteIsNotSearchedOrPassedOnAsRoot)
{
    const FakeTree tree = staffWritableUsrLocal();
    const RootTools::RootFs fs = tree.fs();
    EXPECT_EQ(RootTools::trustedRootPath("/usr/local/bin:/usr/bin", fs), "/usr/bin");
    EXPECT_EQ(RootTools::findRootTool("xdg-open", "/usr/local/bin:/usr/bin", fs), std::optional<std::string>("/usr/bin/xdg-open"))
        << "the planted xdg-open -> python3 is never found: its folder isn't searched";
    EXPECT_EQ(RootTools::findRootTool("crontab", "/usr/local/bin:/usr/bin", fs), std::optional<std::string>("/usr/bin/crontab"));
}

// #4550: a trusted folder is given as the canonical path that was checked, once. A staff member's "bin -> /usr/bin"
// in a staff-writable /usr/local passes on its target, so it must never reach the PATH as /usr/local/bin, which they
// could re-point before a root child resolves a name through it; on merged-usr, /bin is /usr/bin.
TEST(RootToolsTest, TrustedFoldersAreGivenAsTheirCanonicalPathOnce)
{
    FakeTree tree = staffWritableUsrLocal();
    tree.entries.erase("/usr/local/bin");
    tree.entries["/usr/local/bin"] = {.uid = 1000, .mode = 0120777};
    tree.links["/usr/local/bin"] = "/usr/bin";
    tree.entries["/bin"] = {.uid = 0, .mode = 0120777};
    tree.links["/bin"] = "/usr/bin";
    const RootTools::RootFs fs = tree.fs();
    EXPECT_EQ(RootTools::trustedRootPath("/usr/local/bin:/usr/bin:/bin", fs), "/usr/bin");
    EXPECT_EQ(RootTools::trustedRootPath("/bin:/usr/local/bin", fs), "/usr/bin") << "the link's own name is never passed on";
}

// The #4551 review: a hop's path is checked link by link, folders included. A staff member who swaps the folder link
// /usr/local/lib/nodejs for one to another root-only folder holding bin/node would choose which root-owned program
// /usr/bin/node runs; the same hop through root's own links (merged-usr /bin) is fine.
TEST(RootToolsTest, AFolderLinkOnAHopsWayMustSitInARootOnlyFolder)
{
    constexpr std::uint32_t DIR = 0040000;
    FakeTree tree = staffWritableUsrLocal();
    tree.entries["/usr/local/lib"] = {.uid = 0, .mode = DIR | 02775};
    tree.entries["/usr/local/lib/nodejs"] = {.uid = 1000, .mode = 0120777};
    tree.links["/usr/local/lib/nodejs"] = "/opt/other";
    tree.entries["/opt"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/opt/other"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/opt/other/bin"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/opt/other/bin/node"] = {.uid = 0, .mode = 0100755};
    tree.entries["/usr/bin/node"] = {.uid = 0, .mode = 0120777};
    tree.links["/usr/bin/node"] = "/usr/local/lib/nodejs/bin/node";
    tree.entries["/usr/bin/direct"] = {.uid = 0, .mode = 0120777};
    tree.links["/usr/bin/direct"] = "/opt/other/bin/node";
    tree.entries["/bin"] = {.uid = 0, .mode = 0120777};
    tree.links["/bin"] = "/usr";
    tree.entries["/usr/bin/merged"] = {.uid = 0, .mode = 0120777};
    tree.links["/usr/bin/merged"] = "/bin/bin/python3"; // through root's /bin link
    const RootTools::RootFs fs = tree.fs();

    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/bin/node", fs).has_value())
        << "/usr/local/lib/nodejs is a link in a staff-writable folder: it chooses where the hop leads";
    EXPECT_EQ(RootTools::rootTrustedPath("/usr/bin/direct", fs), std::optional<std::string>("/usr/bin/direct")) << "#4531";

    // The #4564 review: a hop through a folder link is answered canonically. /usr/bin/merged's hop goes through root's
    // /bin link (merged-usr), so it runs as the python3 it leads to.
    EXPECT_EQ(RootTools::rootTrustedPath("/usr/bin/merged", fs), std::optional<std::string>("/usr/bin/python3"))
        << "through a folder link: canonical";
    EXPECT_TRUE(RootTools::isPlainAbsolutePath("/usr/bin/pkill"));
    for (const std::string_view odd : {"/usr/bin/../bin/pkill", "/usr/./bin/pkill", "/usr//bin/pkill", "usr/bin/pkill", "/usr/bin/"})
    {
        EXPECT_FALSE(RootTools::isPlainAbsolutePath(odd)) << odd << ": answered in its canonical form instead";
    }
}

// The #4551 review: with no trusted folder, a root child's PATH finds nothing. An empty PATH would search the
// current directory.
// The #4564 review: a link nested in a folder link's target is checked only where it leads. /usr/bin/two -> /opt/app/
// bin/node, where root's /opt/app links into a staff-writable /usr/local/lib/nodejs, a link the staff member can swap:
// run by its own name, the kernel would walk that link again at the exec. It is answered canonically, as is a /proc
// magic link, which changes with the process behind it; a link to its neighbour (pkill -> pgrep) keeps its name.
TEST(RootToolsTest, APathThroughAFolderLinkRunsCanonicallyAndANeighbourLinkKeepsItsName)
{
    constexpr std::uint32_t DIR = 0040000;
    FakeTree tree = staffWritableUsrLocal();
    tree.entries["/usr/local/lib"] = {.uid = 0, .mode = DIR | 02775};
    tree.entries["/usr/local/lib/nodejs"] = {.uid = 1000, .mode = 0120777};
    tree.links["/usr/local/lib/nodejs"] = "/opt/other";
    tree.entries["/opt"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/opt/other"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/opt/other/bin"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/opt/other/bin/node"] = {.uid = 0, .mode = 0100755};
    tree.entries["/opt/app"] = {.uid = 0, .mode = 0120777};
    tree.links["/opt/app"] = "/usr/local/lib/nodejs";
    tree.entries["/usr/bin/two"] = {.uid = 0, .mode = 0120777};
    tree.links["/usr/bin/two"] = "/opt/app/bin/node";
    tree.entries["/usr/bin/pgrep"] = {.uid = 0, .mode = 0100755};
    tree.entries["/usr/bin/pkill"] = {.uid = 0, .mode = 0120777};
    tree.links["/usr/bin/pkill"] = "/usr/bin/pgrep";
    tree.entries["/proc"] = {.uid = 0, .mode = DIR | 0555};
    tree.entries["/proc/1"] = {.uid = 0, .mode = DIR | 0555};
    tree.entries["/proc/1/cwd"] = {.uid = 0, .mode = 0120777};
    tree.links["/proc/1/cwd"] = "/";
    const RootTools::RootFs fs = tree.fs();

    EXPECT_EQ(RootTools::rootTrustedPath("/usr/bin/two", fs), std::optional<std::string>("/opt/other/bin/node"))
        << "never by a name the kernel would walk through the staff member's link again";
    EXPECT_EQ(RootTools::rootTrustedPath("/proc/1/cwd/usr/bin/pgrep", fs), std::optional<std::string>("/usr/bin/pgrep"));
    EXPECT_EQ(RootTools::rootTrustedPath("/usr/bin/pkill", fs), std::optional<std::string>("/usr/bin/pkill"))
        << "#4531: pkill keeps the name it dispatches on";

    // The #4564 re-review: /proc/<pid>/exe is a root-only link in a root-only folder, but leads to whatever the process
    // holding that PID at the exec runs ($TERMINAL=/proc/<pid>/exe of a setuid program, its PID then reused).
    FakeTree procTree = tree;
    procTree.entries["/proc/123"] = {.uid = 0, .mode = DIR | 0555};
    procTree.entries["/proc/123/exe"] = {.uid = 0, .mode = 0120777};
    procTree.links["/proc/123/exe"] = "/usr/bin/pgrep";
    EXPECT_EQ(RootTools::rootTrustedPath("/proc/123/exe", procTree.fs()), std::optional<std::string>("/usr/bin/pgrep"))
        << "never by the magic link's name";
    // procfs mounted elsewhere is found by its filesystem type.
    procTree.entries["/srv"] = {.uid = 0, .mode = DIR | 0755};
    procTree.entries["/srv/p"] = {.uid = 0, .mode = DIR | 0555};
    procTree.entries["/srv/p/exe"] = {.uid = 0, .mode = 0120777};
    procTree.links["/srv/p/exe"] = "/usr/bin/pgrep";
    RootTools::RootFs onProc = procTree.fs();
    onProc.onProcfs = [](const std::string& folder)
    {
        return folder.starts_with("/srv/p");
    };
    EXPECT_EQ(RootTools::rootTrustedPath("/srv/p/exe", onProc), std::optional<std::string>("/usr/bin/pgrep"));
    EXPECT_EQ(RootTools::rootTrustedPath("/srv/p/exe", procTree.fs()), std::optional<std::string>("/srv/p/exe"))
        << "without the filesystem type, the same link would keep its name";
}

// #4554: a FUSE (or NFS, CIFS) filesystem reports whatever owner it likes, root's included: nothing on one is trusted
// as root's, neither a folder to search nor a tool to run.
TEST(RootToolsTest, NothingOnAFilesystemThatMakesUpItsOwnersIsTrusted)
{
    const FakeTree tree = staffWritableUsrLocal();
    RootTools::RootFs fs = tree.fs();
    EXPECT_EQ(RootTools::trustedRootPath("/usr/bin", fs), "/usr/bin");
    EXPECT_TRUE(RootTools::rootTrustedPath("/usr/bin/python3", fs).has_value());
    fs.onUntrustedFilesystem = [](const std::string& path)
    {
        return path.starts_with("/usr/bin");
    }; // a FUSE mount
    EXPECT_EQ(RootTools::trustedRootPath("/usr/bin", fs), "") << "its root:0755 is whatever the mount says";
    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/bin/python3", fs).has_value());
    EXPECT_FALSE(RootTools::findRootTool("xdg-open", "/usr/bin", fs).has_value());
}

// The #4572 review: #4554's own case. A FUSE filesystem on /usr/local/bin reports root:0755 for everything, so the mode
// checks pass; only its filesystem refuses it, as a folder to search, a hop's folder, or anything below it.
TEST(RootToolsTest, AFuseUsrLocalBinReportingRootOwnersIsRefusedOnlyByItsFilesystem)
{
    constexpr std::uint32_t DIR = 0040000;
    FakeTree tree = staffWritableUsrLocal();
    tree.entries["/usr/local"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/usr/local/bin"] = {.uid = 0, .mode = DIR | 0755};
    tree.entries["/usr/local/bin/xdg-open"] = {.uid = 0, .mode = 0120777};
    tree.entries["/usr/local/bin/crontab"] = {.uid = 0, .mode = 0120777};
    RootTools::RootFs fs = tree.fs();
    ASSERT_EQ(RootTools::trustedRootPath("/usr/local/bin:/usr/bin", fs), "/usr/local/bin:/usr/bin") << "the modes alone pass";
    fs.onUntrustedFilesystem = [](const std::string& path)
    {
        return path.starts_with("/usr/local/bin");
    };
    EXPECT_EQ(RootTools::trustedRootPath("/usr/local/bin:/usr/bin", fs), "/usr/bin");
    EXPECT_EQ(RootTools::findRootTool("xdg-open", "/usr/local/bin:/usr/bin", fs), std::optional<std::string>("/usr/bin/xdg-open"));
    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/local/bin/xdg-open", fs).has_value()) << "a hop's folder on FUSE";
    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/bin/hop", fs).has_value()) << "a later hop's folder on FUSE";

    // Only the program itself on an untrusted filesystem: the final check refuses it.
    fs.onUntrustedFilesystem = [](const std::string& path)
    {
        return path == "/usr/bin/python3";
    };
    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/bin/python3", fs).has_value());
    // An untrusted ancestor refuses what's mounted below it.
    fs.onUntrustedFilesystem = [](const std::string& path)
    {
        return path == "/usr";
    };
    EXPECT_EQ(RootTools::trustedRootPath("/usr/bin", fs), "") << "/usr/bin on a trusted mount below an untrusted /usr";
}

TEST(RootToolsTest, WhichFilesystemsCanMakeUpTheirOwners)
{
    constexpr std::uint64_t NOSUID = 2;
    for (const std::uint64_t untrusted :
         {0x65735546ULL, 0x6969ULL, 0xFF534D42ULL, 0xFE534D42ULL, 0x00C36400ULL, 0x5346414FULL, 0x6B414653ULL, 0x01021997ULL})
    {
        EXPECT_TRUE(RootTools::untrustedFilesystem(untrusted, 0)) << std::hex << untrusted;
    }
    EXPECT_FALSE(RootTools::untrustedFilesystem(0xEF53, 0)) << "ext4";
    EXPECT_FALSE(RootTools::untrustedFilesystem(0x794C7630, 0)) << "overlayfs keeps real owners (containers, CI)";
    EXPECT_TRUE(RootTools::untrustedFilesystem(0xEF53, NOSUID)) << "a user's udisks mount of their own ext4 image";
    EXPECT_TRUE(RootTools::untrustedFilesystem(0xFFFFFFFFFF534D42ULL, 0)) << "a sign-extended CIFS magic";
    // The real statfs: a path that isn't there is untrusted, and this machine's / isn't.
    const RootTools::RootFs real = RootTools::systemRootFs();
    EXPECT_TRUE(real.onUntrustedFilesystem("/nonexistent-4554"));
    EXPECT_FALSE(real.onUntrustedFilesystem("/")) << "/ is a root filesystem, mounted with suid";
}

TEST(RootToolsTest, WithNoTrustedFolderTheRootPathFindsNothing)
{
    EXPECT_EQ(RootTools::rootPathVariable(""), "PATH=/proc/nonexistent");
    EXPECT_EQ(RootTools::rootPathVariable("/usr/sbin:/usr/bin"), "PATH=/usr/sbin:/usr/bin");
    FakeTree tree = staffWritableUsrLocal();
    tree.entries["/usr/sbin"] = {.uid = 0, .mode = 0040755};
    const RootTools::RootFs fs = tree.fs();
    EXPECT_EQ(RootTools::trustedRootPath(":/usr/sbin:/usr/bin:.:relative:", fs), "/usr/sbin:/usr/bin")
        << "order kept; empty and relative elements dropped";
    EXPECT_EQ(RootTools::trustedRootPath("/usr/local/bin", fs), "") << "nothing trusted";
}

// On this machine: every trusted folder is absolute, already canonical, and listed once.
TEST(RootToolsTest, TheRealTrustedFoldersAreCanonicalAndUnique)
{
    const std::string trusted = RootTools::trustedRootPath();
    std::vector<std::string> seen;
    for (std::size_t start = 0; start < trusted.size();)
    {
        std::size_t end = trusted.find(':', start);
        end = end == std::string::npos ? trusted.size() : end;
        const std::string folder = trusted.substr(start, end - start);
        std::error_code ec;
        EXPECT_TRUE(folder.starts_with('/')) << folder;
        EXPECT_EQ(std::filesystem::canonical(folder, ec).string(), folder);
        EXPECT_FALSE(std::ranges::contains(seen, folder)) << folder;
        seen.push_back(folder);
        start = end + 1;
    }
}

TEST(RootToolsTest, ALinkInAFolderAnotherAccountCanWriteIsRefusedOnAnyHop)
{
    const FakeTree tree = staffWritableUsrLocal();
    const RootTools::RootFs fs = tree.fs();
    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/local/bin/xdg-open", fs).has_value()) << "its own folder isn't root-only";
    EXPECT_FALSE(RootTools::rootTrustedPath("/usr/bin/hop", fs).has_value()) << "a later hop sits in a staff-writable folder";
    EXPECT_EQ(RootTools::rootTrustedPath("/usr/bin/python3", fs), std::optional<std::string>("/usr/bin/python3"));
    EXPECT_FALSE(RootTools::rootTrustedPath("bin/python3", fs).has_value()) << "relative paths are never trusted";
}

// #3760: a tool TaskSmack runs as root (crontab, every Startup sample) is found only in the system's directories, never
// in a directory of the invoking user's PATH, and runs with an environment of TaskSmack's own; not as root, PATH decides.
TEST(RootToolsTest, AsRootAToolIsNeverFoundOnTheUsersPath)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tasksmack-3760-path";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::filesystem::path planted = dir / "crontab";
    {
        std::ofstream file(planted);
        file << "#!/bin/sh\necho planted\n";
    }
    std::filesystem::permissions(planted, std::filesystem::perms::owner_all, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::string pathEnv = dir.string() + ":/usr/bin:/bin";

    EXPECT_EQ(RootTools::find("crontab", false, pathEnv), planted.string()) << "not root: PATH decides";
    const std::optional<std::string> asRoot = RootTools::find("crontab", true, pathEnv);
    EXPECT_NE(asRoot, planted.string()) << "as root, never the user's directory";
    if (asRoot.has_value())
    {
        EXPECT_TRUE(asRoot->starts_with('/'));
    }
    EXPECT_TRUE(RootTools::find("sh", true, pathEnv).has_value()) << "a system tool is still found as root";
    EXPECT_EQ(RootTools::rootEnvironment(), (std::vector<std::string>{RootTools::rootPathVariable(RootTools::trustedRootPath())}));

    std::filesystem::remove_all(dir, ec);
}

// #3855: a program TaskSmack starts as root and as itself keeps TaskSmack's environment (the display, the session bus)
// but gets the system's PATH, whatever PATH TaskSmack itself was started with.
TEST(RootToolsTest, AsRootAChildKeepsTheEnvironmentButNotThePath)
{
    std::array<const char*, 5> env{
        "DISPLAY=:0", "PATH=/home/someone/bin:/usr/bin", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus", "PATHLIKE=kept", nullptr};
    const std::vector<std::string> out = RootTools::withRootPath(env.data());
    EXPECT_EQ(out,
              (std::vector<std::string>{"DISPLAY=:0",
                                        "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus",
                                        "PATHLIKE=kept",
                                        RootTools::rootPathVariable(RootTools::trustedRootPath())}));
    std::array<const char*, 1> none{nullptr};
    EXPECT_EQ(RootTools::withRootPath(none.data()), (std::vector<std::string>{RootTools::rootPathVariable(RootTools::trustedRootPath())}))
        << "a PATH is set even when there was none";
}

} // namespace
} // namespace Platform::SyscallTrace
