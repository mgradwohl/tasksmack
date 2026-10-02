/// @file test_ProcessName.cpp
/// @brief Tests for Platform::ProcessName::resolveFullName(), which recovers a process name the
/// kernel truncated to 15 characters from the process's command line (#951).

#include "Platform/Linux/ProcessName.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace Platform
{
namespace
{

using ProcessName::argument;
using ProcessName::baseName;
using ProcessName::extendsTruncatedName;
using ProcessName::KERNEL_COMM_MAX;
using ProcessName::resolveFullName;

using namespace std::string_view_literals;

/// Builds a raw /proc/[pid]/cmdline buffer: each argument followed by a NUL, as the kernel does.
[[nodiscard]] std::string rawCmdline(std::initializer_list<std::string_view> args)
{
    std::string raw;
    for (const std::string_view arg : args)
    {
        raw.append(arg);
        raw.push_back('\0');
    }
    return raw;
}

// ========== The cases from the issue ==========

TEST(ProcessNameTest, RecoversNameFromExecutablePath)
{
    const std::string raw = rawCmdline({"/usr/lib/systemd/systemd-journald"});
    EXPECT_EQ(resolveFullName("systemd-journal", raw), "systemd-journald");
}

TEST(ProcessNameTest, RecoversNameWhenExecutableHasArguments)
{
    const std::string raw = rawCmdline({"/usr/lib/systemd/systemd-resolved", "--some-flag"});
    EXPECT_EQ(resolveFullName("systemd-resolve", raw), "systemd-resolved");
}

// A script run through an interpreter: argv[0] is the interpreter, and the kernel took comm from
// the script's file name, which is argv[1].
TEST(ProcessNameTest, RecoversScriptNameFromSecondArgument)
{
    const std::string raw =
        rawCmdline({"/usr/bin/python3", "/usr/share/unattended-upgrades/unattended-upgrade-shutdown", "--wait-for-signal"});
    EXPECT_EQ(resolveFullName("unattended-upgr", raw), "unattended-upgrade-shutdown");
}

TEST(ProcessNameTest, RecoversNameFromBareExecutableName)
{
    const std::string raw = rawCmdline({"a-very-long-program-name", "arg"});
    EXPECT_EQ(resolveFullName("a-very-long-pro", raw), "a-very-long-program-name");
}

// ========== Names that must be left alone ==========

// Only a comm of exactly 15 characters can have been truncated. A shorter one is complete, even if
// the command line happens to hold something longer that starts the same way.
TEST(ProcessNameTest, ShortNameIsNeverReplaced)
{
    const std::string raw = rawCmdline({"/usr/bin/bash-completion-helper"});
    EXPECT_EQ(resolveFullName("bash", raw), "bash");
    EXPECT_EQ(resolveFullName("fourteen-chars", raw), "fourteen-chars");
}

// A process that named itself with prctl(PR_SET_NAME): the command line says something else, so
// the 15-character name is the real one.
TEST(ProcessNameTest, SelfChosenNameIsKept)
{
    const std::string raw = rawCmdline({"/opt/app/bin/server", "--worker"});
    EXPECT_EQ(resolveFullName("worker-thread-7", raw), "worker-thread-7");
}

// A name that is genuinely exactly 15 characters long is returned unchanged.
TEST(ProcessNameTest, ExactlyFifteenCharacterNameIsKept)
{
    const std::string raw = rawCmdline({"/usr/bin/exactly15chars-"});
    ASSERT_EQ("exactly15chars-"sv.size(), KERNEL_COMM_MAX);
    EXPECT_EQ(resolveFullName("exactly15chars-", raw), "exactly15chars-");
}

// The reviewed defect: a program whose name really is 15 characters, run with an argument that
// begins with that name. argv[0] shows the name is complete, so argv[1] must not be consulted.
TEST(ProcessNameTest, CompleteNameIsNotReplacedByAnArgumentThatExtendsIt)
{
    const std::string raw = rawCmdline({"exactly15chars-", "exactly15chars-option"});
    EXPECT_EQ(resolveFullName("exactly15chars-", raw), "exactly15chars-");

    const std::string withPath = rawCmdline({"/usr/local/bin/exactly15chars-", "/etc/exactly15chars-option.conf"});
    EXPECT_EQ(resolveFullName("exactly15chars-", withPath), "exactly15chars-");
}

// argv[1] is only a candidate when argv[0] is unrelated to comm, as with an interpreter.
TEST(ProcessNameTest, SecondArgumentIsConsultedOnlyWhenTheFirstIsUnrelated)
{
    const std::string interpreted = rawCmdline({"/bin/sh", "/opt/tools/a-very-long-script-name.sh"});
    EXPECT_EQ(resolveFullName("a-very-long-scr", interpreted), "a-very-long-script-name.sh");

    // argv[0] already gives the full name: it wins, and argv[1] is not looked at.
    const std::string direct = rawCmdline({"/opt/tools/a-very-long-script-name.sh", "a-very-long-script-name.sh.bak"});
    EXPECT_EQ(resolveFullName("a-very-long-scr", direct), "a-very-long-script-name.sh");
}

// Kernel threads have no command line at all.
TEST(ProcessNameTest, KernelThreadWithEmptyCmdlineIsKept)
{
    EXPECT_EQ(resolveFullName("kworker/u16:3-e", ""), "kworker/u16:3-e");
}

// A program that overwrote its own argv with a status line (no NULs, spaces and slashes inside).
// The base name of that string is not a longer spelling of comm, so comm is kept.
TEST(ProcessNameTest, RewrittenArgvIsNotMistakenForAName)
{
    const std::string raw = "postgres: checkpointer process for /var/lib/postgresql/15/main";
    EXPECT_EQ(resolveFullName("postgres-worker", raw), "postgres-worker");
}

// The interpreter's own name is not a longer spelling of the script's, and a later argument that
// happens to match is not consulted: only argv[0] and argv[1].
TEST(ProcessNameTest, OnlyFirstTwoArgumentsAreConsidered)
{
    const std::string raw = rawCmdline({"/usr/bin/env", "python3", "/opt/tools/a-very-long-script-name.py"});
    EXPECT_EQ(resolveFullName("a-very-long-scr", raw), "a-very-long-scr");
}

TEST(ProcessNameTest, UnterminatedCmdlineIsHandled)
{
    // No trailing NUL after the only argument.
    EXPECT_EQ(resolveFullName("systemd-journal", "/usr/lib/systemd/systemd-journald"), "systemd-journald");
}

TEST(ProcessNameTest, TrailingSlashYieldsNoCandidate)
{
    const std::string raw = rawCmdline({"/usr/lib/systemd-journal/"});
    EXPECT_EQ(resolveFullName("systemd-journal", raw), "systemd-journal");
}

// ========== Building blocks ==========

TEST(ProcessNameTest, BaseNameStripsDirectories)
{
    EXPECT_EQ(baseName("/usr/lib/systemd/systemd-journald"), "systemd-journald");
    EXPECT_EQ(baseName("relative/path/tool"), "tool");
    EXPECT_EQ(baseName("tool"), "tool");
    EXPECT_EQ(baseName("/"), "");
    EXPECT_EQ(baseName(""), "");
}

TEST(ProcessNameTest, ArgumentSplitsOnNul)
{
    const std::string raw = rawCmdline({"first", "second", "third"});
    EXPECT_EQ(argument(raw, 0), "first");
    EXPECT_EQ(argument(raw, 1), "second");
    EXPECT_EQ(argument(raw, 2), "third");
    EXPECT_EQ(argument(raw, 3), "");
    EXPECT_EQ(argument("", 0), "");
    EXPECT_EQ(argument("only", 1), "");
}

TEST(ProcessNameTest, EmptyArgumentIsPreserved)
{
    const std::string raw = rawCmdline({"prog", "", "x"});
    EXPECT_EQ(argument(raw, 1), "");
    EXPECT_EQ(argument(raw, 2), "x");
}

TEST(ProcessNameTest, ExtendsRequiresAStrictlyLongerPrefixMatch)
{
    EXPECT_TRUE(extendsTruncatedName("systemd-journal", "systemd-journald"));
    EXPECT_FALSE(extendsTruncatedName("systemd-journal", "systemd-journal")); // same, not longer
    EXPECT_FALSE(extendsTruncatedName("systemd-journal", "systemd-resolved"));
    EXPECT_FALSE(extendsTruncatedName("systemd-journal", "journald"));
    EXPECT_FALSE(extendsTruncatedName("systemd-journal", ""));
}

} // namespace
} // namespace Platform
