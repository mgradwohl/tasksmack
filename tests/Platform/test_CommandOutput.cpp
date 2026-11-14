/// @file test_CommandOutput.cpp
/// @brief A short command's output (#3425, for `crontab -l`): what it prints and its exit status, a program that
/// isn't there, one that outlives the timeout (killed), a timeout already spent (#3374), one that closes its output and keeps running
/// (killed at the timeout too), output past the byte limit, and an allocation failing part way (the child is still killed and reaped).

#include "Domain/AllocationFailureHook.h"
#include "Platform/Linux/CommandOutput.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <new>

#include <sys/wait.h>

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

// #3374: a timeout already spent when the spawn returns: nothing is read, and the child is still killed and reaped.
TEST(CommandOutputTest, ATimeoutAlreadySpentReadsNothingAndKillsTheChild)
{
    const auto began = std::chrono::steady_clock::now();
    const CommandOutput run = runForOutput({"sh", "-c", "exec sleep 30"}, 0ms, 1024);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 10s) << "killed, not waited for";
    EXPECT_TRUE(run.started);
    EXPECT_TRUE(run.timedOut);
    EXPECT_EQ(run.exitCode, -1);
    EXPECT_TRUE(run.output.empty());
}

// #3441 review: a child that closes its output but keeps running used to reach a blocking waitpid() past the deadline.
TEST(CommandOutputTest, OneThatClosesItsOutputAndKeepsRunningIsKilledAtTheTimeout)
{
    const auto began = std::chrono::steady_clock::now();
    const CommandOutput run = runForOutput({"sh", "-c", "echo bye; exec >&-; exec sleep 30"}, 300ms, 1024);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 10s);
    EXPECT_TRUE(run.started);
    EXPECT_TRUE(run.timedOut);
    EXPECT_EQ(run.exitCode, -1);
    EXPECT_EQ(run.output, "bye\n");
}

// #3441 review: an exception after the spawn (here a real std::bad_alloc from the output growing, through the test
// binary's allocation hook) still kills and reaps the child: no process or zombie of its is left.
TEST(CommandOutputTest, AnAllocationFailureLeavesNoChildBehind)
{
#ifdef TASKSMACK_NO_ALLOCATOR_HOOK
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    int failures = 0;
    for (std::int64_t nth = 0; nth < 64; ++nth)
    {
        bool threw = false;
        {
            const TestSupport::FailAllocationAfter failing(nth);
            try
            {
                // Output in several reads, so the string grows (allocates) after the child is running.
                static_cast<void>(runForOutput({"sh", "-c", "head -c 200000 /dev/zero; exec sleep 30"}, 2s, 1U << 20U));
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
            if (!threw && TestSupport::allocationFailurePending())
            {
                break; // the run made fewer than nth allocations: every one has been failed in turn
            }
        }
        if (threw)
        {
            ++failures;
            int status = 0;
            errno = 0;
            EXPECT_EQ(::waitpid(-1, &status, WNOHANG), -1) << "allocation " << nth << " left a child behind";
            EXPECT_EQ(errno, ECHILD) << "allocation " << nth;
        }
    }
    EXPECT_GT(failures, 1) << "the reserve before the spawn, and at least one append after it";
#endif
}

TEST(CommandOutputTest, OutputPastTheLimitIsDropped)
{
    const CommandOutput run = runForOutput({"sh", "-c", "head -c 100000 /dev/zero | tr '\\0' x"}, 10s, 1000);
    EXPECT_EQ(run.exitCode, 0) << "it still runs to the end";
    EXPECT_EQ(run.output.size(), 1000U);
}

} // namespace
} // namespace Platform
