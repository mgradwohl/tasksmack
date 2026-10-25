/// @file test_LinuxCoreDump.cpp
/// @brief Tests for Platform::LinuxCoreDump (#807), the pure half of "Create dump file" on Linux: the
/// core file's name, the gcore command, what is said when gcore fails, and gcore's ptrace refusals.

#include "Platform/Linux/LinuxCoreDump.h"
#include "Platform/Linux/SyscallTrace.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

[[nodiscard]] std::tm someLocalTime()
{
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 9; // October
    t.tm_mday = 15;
    t.tm_hour = 4;
    t.tm_min = 7;
    t.tm_sec = 9;
    return t;
}

TEST(LinuxCoreDumpTest, TheFileIsNamedForTheProcessItsPidAndTheTime)
{
    using LinuxCoreDump::dumpFileName;
    const std::tm t = someLocalTime();
    EXPECT_EQ(dumpFileName("bash", 1234, t, 1), "bash_1234_2026-10-15_04-07-09.core");
    EXPECT_EQ(dumpFileName("bash", 1234, t, 2), "bash_1234_2026-10-15_04-07-09 (2).core") << "the second try when the first exists";
    EXPECT_EQ(dumpFileName("bash", 1234, t, 17), "bash_1234_2026-10-15_04-07-09 (17).core");
}

TEST(LinuxCoreDumpTest, ANameThatCantBeAFileNameIsMadeOne)
{
    using LinuxCoreDump::dumpFileName;
    const std::tm t = someLocalTime();
    // comm can hold almost anything: a '/' would make a path, a control character an unreadable name.
    EXPECT_EQ(dumpFileName("kworker/0:1", 7, t, 1), "kworker_0:1_7_2026-10-15_04-07-09.core");
    EXPECT_EQ(dumpFileName(std::string("a\nb\x7F", 4), 7, t, 1), "a_b__7_2026-10-15_04-07-09.core");
    EXPECT_EQ(dumpFileName("", 7, t, 1), "process_7_2026-10-15_04-07-09.core");
    EXPECT_EQ(dumpFileName("..", 7, t, 1), "process_7_2026-10-15_04-07-09.core");
}

TEST(LinuxCoreDumpTest, GcoreWritesThePrefixDotPid)
{
    EXPECT_EQ(LinuxCoreDump::gcoreArgv("/usr/bin/gcore", "/tmp/dumps/x.core.partial", 42),
              (std::vector<std::string>{"/usr/bin/gcore", "-o", "/tmp/dumps/x.core.partial", "42"}));
    EXPECT_EQ(LinuxCoreDump::gcoreOutputFile("/tmp/dumps/x.core.partial", 42), std::filesystem::path("/tmp/dumps/x.core.partial.42"));
}

TEST(LinuxCoreDumpTest, AFailureSaysWhatGcoreSaidLast)
{
    using LinuxCoreDump::gcoreFailureMessage;
    EXPECT_EQ(gcoreFailureMessage(42, 1, "warning: something\nptrace: Operation not permitted.\n\n"),
              "gcore couldn't write a dump of process 42: ptrace: Operation not permitted.");
    EXPECT_EQ(gcoreFailureMessage(42, 1, "   \n"), "gcore couldn't write a dump of process 42 (exit status 1)");
    EXPECT_EQ(gcoreFailureMessage(42, -1, ""), "gcore couldn't write a dump of process 42 (exit status -1)");
    EXPECT_EQ(LinuxCoreDump::lastLine("  only line  "), "only line");
    EXPECT_EQ(LinuxCoreDump::lastLine("first\n  \n\t\n"), "first");
}

TEST(LinuxCoreDumpTest, ACoreHoldsTheAnonymousSharedAndSwappedMemory)
{
    using LinuxCoreDump::coreBytesFromStatus;
    // Lines as the kernel writes them, a few left out.
    const std::string status = "Name:\tapp\nVmRSS:\t   10000 kB\nRssAnon:\t    6000 kB\nRssFile:\t    3000 kB\n"
                               "RssShmem:\t    1000 kB\nVmSwap:\t     500 kB\n";
    EXPECT_EQ(coreBytesFromStatus(status), std::optional<std::uint64_t>{(6000U + 1000U + 500U) * 1024U});
    // No shared memory or swap reported: the anonymous memory alone.
    EXPECT_EQ(coreBytesFromStatus("Name:\tapp\nRssAnon:\t 8 kB\n"), std::optional<std::uint64_t>{8U * 1024U});
    // A kernel thread (no Rss* lines), an old kernel, or a value that isn't one: unknown.
    EXPECT_EQ(coreBytesFromStatus("Name:\tkworker/0:1\n"), std::nullopt);
    EXPECT_EQ(coreBytesFromStatus("RssAnon:\t lots\n"), std::nullopt);
    // A key that only starts the same is not the key.
    EXPECT_EQ(coreBytesFromStatus("RssAnonymous:\t 8 kB\n"), std::nullopt);
}

TEST(LinuxCoreDumpTest, GcoresPtraceRefusalsSayDumpNotTrace)
{
    using SyscallTrace::GCORE_TOOL;
    using SyscallTrace::ptraceRefusal;
    const std::string scope1 =
        ptraceRefusal(42, {.ptraceScope = 1, .sameCredentials = true, .privileged = false}, GCORE_TOOL).value_or(std::string{});
    EXPECT_TRUE(scope1.contains("so gcore cannot attach to process 42")) << scope1;
    EXPECT_TRUE(scope1.contains("give gcore CAP_SYS_PTRACE")) << scope1;
    EXPECT_TRUE(scope1.ends_with("to dump it")) << scope1;

    const std::string otherUser =
        ptraceRefusal(42, {.ptraceScope = 0, .sameCredentials = false, .privileged = false}, GCORE_TOOL).value_or(std::string{});
    EXPECT_TRUE(otherUser.contains("dumping it needs CAP_SYS_PTRACE")) << otherUser;
    EXPECT_TRUE(otherUser.contains("or a gcore that carries it")) << otherUser;

    const std::string scope3 =
        ptraceRefusal(42, {.ptraceScope = 3, .sameCredentials = true, .privileged = true}, GCORE_TOOL).value_or(std::string{});
    EXPECT_TRUE(scope3.ends_with("so gcore cannot dump process 42")) << scope3;

    EXPECT_EQ(ptraceRefusal(42, {.ptraceScope = 1, .sameCredentials = true, .privileged = true}, GCORE_TOOL), std::nullopt);
}

// #2122: a process gdb still holds shows 't', which is a stop as much as 'T'.
TEST(LinuxCoreDumpTest, StatesAndTracersComeFromProc)
{
    using LinuxCoreDump::isStoppedState;
    using LinuxCoreDump::stateFromStat;
    using LinuxCoreDump::tracerPidFromStatus;
    EXPECT_EQ(stateFromStat("42 (a) b) t 1 42"), std::optional<char>{'t'}); // comm with ')' in it
    EXPECT_EQ(stateFromStat("42 (bash) S 1"), std::optional<char>{'S'});
    EXPECT_EQ(stateFromStat("42 (bash)"), std::nullopt);
    EXPECT_EQ(stateFromStat(""), std::nullopt);
    EXPECT_TRUE(isStoppedState('T'));
    EXPECT_TRUE(isStoppedState('t'));
    EXPECT_FALSE(isStoppedState('S'));
    EXPECT_FALSE(isStoppedState('R'));
    EXPECT_EQ(tracerPidFromStatus("Name:\tapp\nState:\tt (tracing stop)\nTracerPid:\t4321\n"), std::optional<std::int32_t>{4321});
    EXPECT_EQ(tracerPidFromStatus("TracerPid:\t0\n"), std::optional<std::int32_t>{0});
    EXPECT_EQ(tracerPidFromStatus("TracerPid:\tx\n"), std::nullopt);
    EXPECT_EQ(tracerPidFromStatus("Name:\tapp\n"), std::nullopt);
}

// #2148: the ptrace privilege that counts is gdb's, the gdb beside the gcore script.
TEST(LinuxCoreDumpTest, GcoreRunsTheGdbBesideIt)
{
    EXPECT_EQ(LinuxCoreDump::gcoreDebuggerPath("/usr/bin/gcore"), "/usr/bin/gdb");
    EXPECT_EQ(LinuxCoreDump::gcoreDebuggerPath("/opt/gdb-15/bin/gcore"), "/opt/gdb-15/bin/gdb");
}

} // namespace
} // namespace Platform
