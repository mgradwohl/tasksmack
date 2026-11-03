/// @file test_WindowsSignatureWorker.cpp
/// @brief The process probe's signature worker (#3070) through fake checks: a request is answered on a
/// later find(), never by the caller's thread; a path is queued once however many processes run it; a
/// new process of an image re-reads its stamp, checked again only when the file changed or has no file ID
/// (#3292); the path table's bound, which a table full of queued paths keeps by admitting no more (#3340),
/// also while a slow check keeps finishing one file at a time under load (#3342); more paths than the
/// table holds, polled in a fixed order, each checked in turn (#3403); a check that throws; and
/// the destructor stopping a queue.
/// Pure (no Windows header), so built everywhere.

#include "Platform/ImageSignature.h"
#include "Platform/Windows/WindowsImageSignature.h"
#include "Platform/Windows/WindowsSignatureWorker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Platform::WindowsSignature
{
namespace
{

using namespace std::chrono_literals;

constexpr auto WAIT = 10s; // generous: a loaded CI runner

struct Fakes
{
    std::mutex mutex;
    std::map<std::string, FileStamp> stamps; // guarded; a path missing here can't be read
    std::map<std::string, int> checks;       // guarded: verifyEmbeddedSignature calls by path
    std::atomic<bool> throwOnCheck{false};
    std::thread::id checkThread;
    // A gate the check waits on while closed, so a test can hold the worker mid-check.
    std::condition_variable gateChanged;
    bool gateOpen = true;
    // A slow verifier (#3340, #3342): while metered, each check, once entered, waits for a permit, so a
    // test lets the worker finish one check at a time while it keeps asking for more.
    bool metered = false;
    std::size_t permits = 0;
    std::size_t entered = 0;   // checks begun while metered
    std::size_t completed = 0; // checks finished while metered
    std::atomic<bool> started{false};
};

Fakes& fakes()
{
    static Fakes state;
    return state;
}

void resetFakes()
{
    const std::scoped_lock lock(fakes().mutex);
    fakes().stamps.clear();
    fakes().checks.clear();
    fakes().throwOnCheck = false;
    fakes().checkThread = {};
    fakes().gateOpen = true;
    fakes().metered = false;
    fakes().permits = 0;
    fakes().entered = 0;
    fakes().completed = 0;
    fakes().started = false;
}

SignatureFunctions fakeFunctions()
{
    return {
        .errorMessage = nullptr,
        .openImage = [](const std::string& path) -> std::optional<OpenedImage>
        {
            const std::scoped_lock lock(fakes().mutex);
            const auto found = fakes().stamps.find(path);
            return found != fakes().stamps.end() ? std::optional<OpenedImage>(OpenedImage{.file = nullptr, .stamp = found->second})
                                                 : std::nullopt;
        },
        .closeImage = [](void* /*file*/) {},
        .verifyEmbeddedSignature =
            [](const std::string& path, void* /*file*/)
        {
            std::unique_lock lock(fakes().mutex);
            fakes().gateChanged.wait(lock, [] { return fakes().gateOpen; });
            if (fakes().metered)
            {
                ++fakes().entered;
                fakes().gateChanged.notify_all();
                fakes().gateChanged.wait(lock, [] { return !fakes().metered || fakes().permits != 0; });
                if (fakes().metered)
                {
                    --fakes().permits;
                }
                ++fakes().completed;
            }
            ++fakes().checks[path];
            fakes().checkThread = std::this_thread::get_id();
            if (fakes().throwOnCheck)
            {
                throw std::runtime_error("the check threw");
            }
            return TrustCheck{.result = 0, .signer = "Signer of " + path};
        },
        .verifyCatalogSignature = [](const std::string& /*path*/, void* /*file*/) -> std::optional<TrustCheck> { return std::nullopt; },
    };
}

void setStamp(const std::string& path, std::uint64_t changeTime)
{
    const std::scoped_lock lock(fakes().mutex);
    fakes().stamps[path] = FileStamp{.fileId = FileId{.volumeSerial = 1, .id = {1}}, .changeTime = changeTime, .size = 100, .usn = 1};
}

int checksOf(const std::string& path)
{
    const std::scoped_lock lock(fakes().mutex);
    return fakes().checks[path];
}

void setGate(bool open)
{
    {
        const std::scoped_lock lock(fakes().mutex);
        fakes().gateOpen = open;
    }
    fakes().gateChanged.notify_all();
}

/// Turns the slow verifier on or off; off lets every check through at once.
void setMetered(bool metered)
{
    {
        const std::scoped_lock lock(fakes().mutex);
        fakes().metered = metered;
    }
    fakes().gateChanged.notify_all();
}

/// Lets @p count more metered checks finish.
void allowChecks(std::size_t count)
{
    {
        const std::scoped_lock lock(fakes().mutex);
        fakes().permits += count;
    }
    fakes().gateChanged.notify_all();
}

/// Waits until @p count metered checks have begun. False if that doesn't happen within WAIT.
[[nodiscard]] bool waitForChecksEntered(std::size_t count)
{
    std::unique_lock lock(fakes().mutex);
    return fakes().gateChanged.wait_for(lock, WAIT, [count] { return fakes().entered >= count; });
}

std::size_t checksCompleted()
{
    const std::scoped_lock lock(fakes().mutex);
    return fakes().completed;
}

class SignatureWorkerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        resetFakes();
    }
    void TearDown() override
    {
        setGate(true); // never leave a worker blocked in a check
        setMetered(false);
    }
};

TEST_F(SignatureWorkerTest, ARequestIsAnsweredLaterOnTheWorkersOwnThread)
{
    setStamp("C:\\app.exe", 1);
    WorkerOptions options;
    options.onWorkerStart = []
    {
        fakes().started = true;
    };
    SignatureWorker worker(fakeFunctions(), options);
    EXPECT_FALSE(worker.find("C:\\app.exe").has_value()) << "an unknown path is requested, not checked here";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    const std::optional<SignatureCheck> check = worker.find("C:\\APP.EXE");
    ASSERT_TRUE(check.has_value()) << "the same path in another case";
    EXPECT_EQ(check->signature, ImageSignature::Embedded);
    EXPECT_EQ(check->signer, "Signer of C:\\app.exe");
    EXPECT_TRUE(fakes().started.load());
    const std::scoped_lock lock(fakes().mutex);
    EXPECT_NE(fakes().checkThread, std::this_thread::get_id());
}

TEST_F(SignatureWorkerTest, APathIsQueuedOnceHoweverManyProcessesAskForIt)
{
    setStamp("svchost.exe", 1);
    setGate(false); // hold the first check
    SignatureWorker worker(fakeFunctions(), {});
    for (int process = 0; process < 30; ++process)
    {
        worker.request("svchost.exe");
        EXPECT_FALSE(worker.find("svchost.exe").has_value());
    }
    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(checksOf("svchost.exe"), 1);
    EXPECT_TRUE(worker.find("svchost.exe").has_value());
    EXPECT_EQ(worker.pathCount(), 1U);
}

TEST_F(SignatureWorkerTest, ANewProcessReReadsTheStampAndAChangedFileIsCheckedAgain)
{
    setStamp("tool.exe", 1);
    SignatureWorker worker(fakeFunctions(), {});
    worker.request("tool.exe");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    ASSERT_TRUE(worker.find("tool.exe").has_value());

    worker.request("tool.exe"); // another process of the same, unchanged file: from the cache
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find("tool.exe").has_value());
    EXPECT_EQ(checksOf("tool.exe"), 1);
    EXPECT_EQ(worker.cachedFileCount(), 1U);

    setStamp("tool.exe", 2); // updated in place
    worker.request("tool.exe");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find("tool.exe").has_value());
    EXPECT_EQ(checksOf("tool.exe"), 2);
}

TEST_F(SignatureWorkerTest, AFileWithoutAFileIdIsCheckedForEveryNewProcess)
{
    {
        const std::scoped_lock lock(fakes().mutex);
        fakes().stamps["E:\\tool.exe"] = FileStamp{.fileId = std::nullopt, .changeTime = 1, .size = 100, .usn = 0}; // FAT
    }
    SignatureWorker worker(fakeFunctions(), {});
    for (int process = 1; process <= 2; ++process)
    {
        worker.request("E:\\tool.exe");
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
        EXPECT_TRUE(worker.find("E:\\tool.exe").has_value());
        EXPECT_EQ(checksOf("E:\\tool.exe"), process);
    }
    EXPECT_EQ(worker.cachedFileCount(), 0U);
}

TEST_F(SignatureWorkerTest, AnUnreadableFileIsAnsweredUnknown)
{
    SignatureWorker worker(fakeFunctions(), {});
    worker.request("gone.exe");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    const std::optional<SignatureCheck> check = worker.find("gone.exe");
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->signature, ImageSignature::Unknown);
    EXPECT_EQ(worker.cachedFileCount(), 0U) << "a failure isn't kept";
}

TEST_F(SignatureWorkerTest, ACheckThatThrowsIsAnsweredUnknownWithItsMessage)
{
    setStamp("bad.exe", 1);
    fakes().throwOnCheck = true;
    SignatureWorker worker(fakeFunctions(), {});
    worker.request("bad.exe");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    const std::optional<SignatureCheck> check = worker.find("bad.exe");
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->signature, ImageSignature::Unknown);
    EXPECT_EQ(check->note, "the check threw");
}

TEST_F(SignatureWorkerTest, AFullPathTableForgetsAnsweredPathsAndFindAsksAgain)
{
    for (const char* path : {"a", "b", "c"})
    {
        setStamp(path, 1);
    }
    WorkerOptions options;
    options.maxPaths = 2;
    SignatureWorker worker(fakeFunctions(), options);
    worker.request("a");
    worker.request("b");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.pathCount(), 2U);
    worker.request("c"); // full: a and b, answered, are forgotten
    EXPECT_EQ(worker.pathCount(), 1U);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find("c").has_value());
    EXPECT_FALSE(worker.find("a").has_value()) << "forgotten: requested again";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find("a").has_value());
    EXPECT_EQ(checksOf("a"), 1) << "its file's answer was still cached";
}

TEST_F(SignatureWorkerTest, ATableFullOfQueuedPathsAdmitsNoMoreUntilTheyAreChecked)
{
    for (const char* path : {"a", "b", "c", "d"})
    {
        setStamp(path, 1);
    }
    setGate(false); // the worker holds a's check: nothing is answered, so nothing can be forgotten
    WorkerOptions options;
    options.maxPaths = 2;
    SignatureWorker worker(fakeFunctions(), options);
    worker.request("a");
    worker.request("b");
    for (int sample = 0; sample < 50; ++sample)
    {
        // Full of queued paths (#3340): c and d aren't admitted, however often they're asked for
        worker.request("c");
        EXPECT_FALSE(worker.find("d").has_value()) << "not checked yet";
    }
    EXPECT_EQ(worker.pathCount(), 2U) << "the table and its queue stay within their bound";
    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(checksOf("c"), 0) << "never queued";
    EXPECT_EQ(checksOf("d"), 0);
    EXPECT_TRUE(worker.find("a").has_value());
    EXPECT_TRUE(worker.find("b").has_value());

    // Once checks have drained the queue, a later sample's find() admits them.
    EXPECT_FALSE(worker.find("c").has_value());
    EXPECT_FALSE(worker.find("d").has_value());
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find("c").has_value());
    EXPECT_TRUE(worker.find("d").has_value());
    EXPECT_EQ(checksOf("c"), 1);
    EXPECT_EQ(checksOf("d"), 1);
    EXPECT_EQ(worker.pathCount(), 2U);
}

TEST_F(SignatureWorkerTest, ASlowCheckThatKeepsMakingProgressKeepsTheTableWithinItsBoundAndChecksEveryPath)
{
    // #3340 under load (#3342): not a worker held still, but one that finishes a check every sample while
    // the probe asks for far more paths than it can check. The table never grows past its bound, a check
    // finishes every round, and once the load stops every path asked for is checked in the end.
    constexpr std::size_t MAX_PATHS = 8;
    constexpr std::size_t ROUNDS = 10;
    constexpr std::size_t NEW_PATHS_A_ROUND = 32;
    std::vector<std::string> paths;
    const auto addPath = [&paths]
    {
        paths.push_back("slow-" + std::to_string(paths.size()) + ".exe");
        setStamp(paths.back(), 1);
        return paths.back();
    };
    setMetered(true);
    WorkerOptions options;
    options.maxPaths = MAX_PATHS;
    SignatureWorker worker(fakeFunctions(), options);
    const struct Unmeter
    {
        Unmeter() = default;
        Unmeter(const Unmeter&) = delete;
        Unmeter& operator=(const Unmeter&) = delete;
        Unmeter(Unmeter&&) = delete;
        Unmeter& operator=(Unmeter&&) = delete;
        // A lock that throws here ends the test run, which is what it should do
        ~Unmeter() // NOLINT(bugprone-exception-escape)
        {
            setMetered(false); // before the worker joins, even when an assertion ends the test early
        }
    } unmeter;

    worker.request(addPath());
    ASSERT_TRUE(waitForChecksEntered(1));
    for (std::size_t round = 1; round <= ROUNDS; ++round)
    {
        for (std::size_t i = 0; i < NEW_PATHS_A_ROUND; ++i)
        {
            worker.request(addPath());
        }
        EXPECT_LE(worker.pathCount(), MAX_PATHS) << "round " << round;
        allowChecks(1); // the check in progress finishes...
        ASSERT_TRUE(waitForChecksEntered(round + 1)) << "...and the next begins: the worker keeps making progress, round " << round;
        EXPECT_EQ(checksCompleted(), round);
    }
    EXPECT_LE(worker.pathCount(), MAX_PATHS);

    // The load stops. Each later sample's find() for a path not checked yet admits it once there's room, a
    // table's worth a sample, until every one has been checked. (Only those are asked for: a probe asking
    // for more images than the table holds, always in the same order, would re-admit the first ones each
    // sample.)
    setMetered(false);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    const auto allChecked = [&paths]
    {
        return std::ranges::all_of(paths, [](const std::string& path) { return checksOf(path) != 0; });
    };
    const std::size_t maxSamples = paths.size(); // far more than needed: a table's worth a sample
    std::size_t samples = 0;
    for (; samples < maxSamples && !allChecked(); ++samples)
    {
        for (const std::string& path : paths)
        {
            if (checksOf(path) == 0)
            {
                (void) worker.find(path);
            }
        }
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
        EXPECT_LE(worker.pathCount(), MAX_PATHS);
    }
    EXPECT_TRUE(allChecked()) << "every path asked for is checked once the queue drains, after " << samples << " samples";
    for (const std::string& path : paths)
    {
        EXPECT_EQ(checksOf(path), 1) << path << " is checked once, its answer then cached";
    }
}

TEST_F(SignatureWorkerTest, MorePathsThanTheTableHoldsPolledInAFixedOrderAreEachCheckedInTurn)
{
    // #3403: a probe that asks for more images than the table holds, every sample in the same order. Admission
    // resumes each sample where the last one stopped, a table's worth at a time, so the later paths are
    // checked too rather than the first ones being admitted again each sample, and the table keeps its bound.
    constexpr std::size_t MAX_PATHS = 4;
    constexpr std::size_t PATH_COUNT = (4 * MAX_PATHS) + 1; // more than twice the bound: the first ones can't hide it
    constexpr std::size_t SAMPLES = ((PATH_COUNT + MAX_PATHS - 1) / MAX_PATHS) + 1;
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < PATH_COUNT; ++i)
    {
        paths.push_back("image-" + std::to_string(i) + ".exe");
        setStamp(paths.back(), 1);
    }
    WorkerOptions options;
    options.maxPaths = MAX_PATHS;
    SignatureWorker worker(fakeFunctions(), options);
    for (std::size_t sample = 1; sample <= SAMPLES; ++sample)
    {
        for (const std::string& path : paths)
        {
            (void) worker.find(path);
            EXPECT_LE(worker.pathCount(), MAX_PATHS) << "sample " << sample;
        }
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    }
    for (const std::string& path : paths)
    {
        EXPECT_NE(checksOf(path), 0) << path << " is checked within " << SAMPLES << " samples";
    }
}

TEST_F(SignatureWorkerTest, APathWaitingItsTurnThatNobodyAsksForAgainDoesNotHoldUpTheOthers)
{
    // #3403: the path admission resumes with belongs to a process that has gone. Once the probe has come
    // round without asking for it, the others are admitted again.
    for (const char* path : {"a", "gone", "c"})
    {
        setStamp(path, 1);
    }
    setGate(false);
    WorkerOptions options;
    options.maxPaths = 1;
    SignatureWorker worker(fakeFunctions(), options);
    worker.request("a");
    EXPECT_FALSE(worker.find("gone").has_value()) << "turned away: the table is full of a, queued";
    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));

    EXPECT_FALSE(worker.find("c").has_value()) << "not its turn: gone was turned away first";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(checksOf("c"), 0);
    EXPECT_FALSE(worker.find("c").has_value()) << "the next sample, without gone: c is admitted";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(checksOf("c"), 1);
    EXPECT_TRUE(worker.find("c").has_value());
    EXPECT_EQ(checksOf("gone"), 0);
    EXPECT_EQ(worker.pathCount(), 1U);
}

TEST_F(SignatureWorkerTest, TheDestructorStopsTheQueueAndAPauseEndsAtTheStop)
{
    for (const char* path : {"one", "two", "three"})
    {
        setStamp(path, 1);
    }
    setGate(false);
    {
        WorkerOptions options;
        options.pause = std::chrono::hours(1);
        SignatureWorker worker(fakeFunctions(), options);
        worker.request("one");
        worker.request("two");
        worker.request("three");
        setGate(true); // the first check finishes, then the worker pauses for an hour
        while (checksOf("one") == 0)
        {
            std::this_thread::yield();
        }
    } // joined at once: the pause ends at the stop, and the rest of the queue is dropped
    EXPECT_EQ(checksOf("two"), 0);
    EXPECT_EQ(checksOf("three"), 0);
}

TEST_F(SignatureWorkerTest, AnIdleWorkerIsIdleAndStopsWithoutARequest)
{
    SignatureWorker worker(fakeFunctions(), {});
    EXPECT_TRUE(worker.waitUntilIdle(0ms));
    EXPECT_EQ(worker.pathCount(), 0U);
}

} // namespace
} // namespace Platform::WindowsSignature
