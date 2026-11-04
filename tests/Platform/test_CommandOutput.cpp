/// @file test_CommandOutput.cpp
/// @brief A short command's output (#3425, for `crontab -l`): what it prints and its exit status, a program that
/// isn't there, one that outlives the timeout (killed), and output past the byte limit.

#include "Platform/Linux/CommandOutput.h"

#include <gtest/gtest.h>

#include <chrono>

namespace Platform
{
namespace
{

using namespace std::chrono_literals;

TEST(CommandOutputTest, KeepsWhatItPrintsAndItsExitStatus)
{
    CommandOutput run = runForOutput({"sh", "-c", "printf 'one\\ntwo\\n'; echo ignored >&2"}, 5s, 1024);
    EXPECT_TRUE(run.started);
    EXPECT_FALSE(run.timedOut);
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_EQ(run.output, "one\ntwo\n") << "stderr isn't kept";

    run = runForOutput({"sh", "-c", "echo no crontab >&2; exit 1"}, 5s, 1024);
    EXPECT_TRUE(run.started);
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_TRUE(run.output.empty());
}

TEST(CommandOutputTest, AProgramThatIsntThereDoesntStart)
{
    const CommandOutput run = runForOutput({"tasksmack-no-such-program-3425"}, 5s, 1024);
    EXPECT_FALSE(run.started);
    EXPECT_EQ(run.exitCode, -1);
    EXPECT_FALSE(runForOutput({}, 5s, 1024).started);
}

TEST(CommandOutputTest, OneThatOutlivesTheTimeoutIsKilled)
{
    const auto began = std::chrono::steady_clock::now();
    const CommandOutput run = runForOutput({"sh", "-c", "echo started; exec sleep 30"}, 300ms, 1024);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 10s);
    EXPECT_TRUE(run.started);
    EXPECT_TRUE(run.timedOut);
    EXPECT_EQ(run.exitCode, -1);
    EXPECT_EQ(run.output, "started\n") << "what it printed before it was killed";
}

TEST(CommandOutputTest, OutputPastTheLimitIsDropped)
{
    const CommandOutput run = runForOutput({"sh", "-c", "head -c 100000 /dev/zero | tr '\\0' x"}, 10s, 1000);
    EXPECT_EQ(run.exitCode, 0) << "it still runs to the end";
    EXPECT_EQ(run.output.size(), 1000U);
}

} // namespace
} // namespace Platform
