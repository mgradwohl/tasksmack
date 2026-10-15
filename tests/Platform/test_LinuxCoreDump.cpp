/// @file test_LinuxCoreDump.cpp
/// @brief Tests for Platform::LinuxCoreDump (#807), the pure half of "Create dump file" on Linux: the
/// core file's name, the gcore command, what is said when gcore fails, and gcore's ptrace refusals.

#include "Platform/Linux/LinuxCoreDump.h"
#include "Platform/Linux/SyscallTrace.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace Platform
