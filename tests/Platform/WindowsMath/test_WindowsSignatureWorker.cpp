/// @file test_WindowsSignatureWorker.cpp
/// @brief The process probe's signature worker (#3070) through fake checks: a request is answered on a
/// later find(), never by the caller's thread; a path is queued once however many processes run it; a
/// new process of an image re-reads its stamp, checked again only when the file changed or has no file ID
/// (#3292); the path table's bound, which a table full of queued paths keeps by admitting no more (#3340),
/// also while a slow check keeps finishing one file at a time under load (#3342); more paths than the
/// table holds, polled in a fixed order, each checked in turn (#3403); the count of queued paths a full
/// table turns a path away on without sweeping (#3418); a check that throws; and
/// the destructor stopping a queue. On Domain::Worker (#3343): a check that throws something other than a
/// std::exception fails the worker, which answers every path rather than terminating; a burst against a
/// drain in progress, each path checked once; a drain refused; and shutdown while a check is in progress.
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
#include <memory>
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
    std::atomic<bool> throwNonException{false}; // a throw that isn't a std::exception (#3343)
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
    fakes().throwNonException = false;
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
            if (fakes().throwNonException)
            {
                // NOLINTNEXTLINE(bugprone-std-exception-baseclass,hicpp-exception-baseclass) - the point: what a catch (std::exception) misses
                throw 42;
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

/// Meters the checks for as long as it lives, and lets them all through when it goes. Declared after the
/// worker, it goes first, so the worker never joins on a metered check, even when an assertion ends the test.
class MeteredChecks
{
  public:
    MeteredChecks()
    {
        setMetered(true);
    }
    // A lock that throws here ends the test run, which is what it should do
    ~MeteredChecks() // NOLINT(bugprone-exception-escape)
    {
        setMetered(false);
    }
    MeteredChecks(const MeteredChecks&) = delete;
    MeteredChecks& operator=(const MeteredChecks&) = delete;
    MeteredChecks(MeteredChecks&&) = delete;
    MeteredChecks& operator=(MeteredChecks&&) = delete;
};

/// Waits until @p done answers true, or WAIT passes; whether it did. A wait on an outcome the worker
/// leaves, not a sleep standing in for one.
template<typename Done> [[nodiscard]] bool waitUntil(Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
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

TEST_F(SignatureWorkerTest, TheQueuedCountFollowsEveryAdmissionAnswerEvictionAndThrow)
{
    // #3418: a table full of queued paths turns a new one away without sweeping it, on the strength of the
    // worker's count of queued paths. That count must match the table through every way a path is queued
    // (admitted, or an answered one asked for again) and stops being queued (answered, or the check threw),
    // and through a sweep that forgets the answered ones.
    for (const char* path : {"a", "b", "c", "d", "e", "f", "g"})
    {
        setStamp(path, 1);
    }
    setGate(false); // the worker holds a's check: a stays queued while it is checked
    WorkerOptions options;
    options.maxPaths = 2;
    SignatureWorker worker(fakeFunctions(), options);
    const auto expectCounts = [&worker](std::size_t queued, std::size_t paths, const char* step)
    {
        EXPECT_EQ(worker.queuedPathCount(), queued) << step;
        EXPECT_EQ(worker.pathCount(), paths) << step;
    };
    // With the gate open, or for a path whose answer is cached (which never reaches the gate), the worker
    // could answer a path just admitted before its count is read. Holding the fakes' lock stops the worker
    // in openImage() until the count has been read; it is let go before waiting for the worker.
    const auto holdTheWorker = []
    {
        return std::unique_lock(fakes().mutex);
    };
    expectCounts(0, 0, "a new worker");
    worker.request("a");
    worker.request("a");
    expectCounts(1, 1, "a admitted once, however often it's asked for");
    worker.request("b");
    expectCounts(2, 2, "b admitted: the table is full of queued paths");
    worker.request("c");
    EXPECT_FALSE(worker.find("d").has_value());
    expectCounts(2, 2, "c and d turned away: nothing queued can be forgotten");

    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    expectCounts(0, 2, "a and b answered");
    {
        const auto held = holdTheWorker();
        worker.request("a");
        expectCounts(1, 2, "an answered path asked for again is queued again, in its own entry");
    }
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    expectCounts(0, 2, "answered again");

    {
        const auto held = holdTheWorker();
        worker.request("c");
        expectCounts(1, 1, "full of answered paths: the sweep forgets a and b, and c is admitted");
    }
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    fakes().throwOnCheck = true;
    worker.request("d");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    expectCounts(0, 2, "a check that threw is answered, no longer queued");
    EXPECT_EQ(worker.find("d").value_or(SignatureCheck{}).note, "the check threw");
    fakes().throwOnCheck = false;

    setGate(false);
    worker.request("e");
    expectCounts(1, 1, "the sweep forgets c and d, answered, and e is admitted");
    worker.request("f");
    worker.request("g");
    expectCounts(2, 2, "f admitted, g turned away");
    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    expectCounts(0, 2, "e and f answered");
    EXPECT_EQ(checksOf("g"), 0) << "never queued";
    {
        const auto held = holdTheWorker();
        EXPECT_FALSE(worker.find("g").has_value());
        expectCounts(1, 1, "g's turn: admitted once there's room");
    }
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    expectCounts(0, 1, "g answered");
    EXPECT_TRUE(worker.find("g").has_value());
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
    // #3403: a probe that asks for more images than the table holds, every sample in the same order, each
    // image run by two processes (so asked for twice). Admission resumes each sample where the last one
    // stopped, a table's worth at a time, so the later paths are checked too rather than the first ones being
    // admitted again each sample, and the table keeps its bound.
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
        worker.beginSample();
        setGate(false); // no check finishes during the sample, so what it admits doesn't depend on timing
        for (const std::string& path : paths)
        {
            for (int process = 0; process < 2; ++process)
            {
                (void) worker.find(path);
                EXPECT_LE(worker.pathCount(), MAX_PATHS) << "sample " << sample;
            }
        }
        setGate(true);
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    }
    for (const std::string& path : paths)
    {
        EXPECT_NE(checksOf(path), 0) << path << " is checked within " << SAMPLES << " samples";
    }
}

TEST_F(SignatureWorkerTest, RepeatedImagesPolledInAFixedOrderDoNotStarveTheLaterOnes)
{
    // #3403 review: four images, each run by two processes, with room for one. Every image is checked in
    // turn: a second process of the same image in the same sample isn't taken for the start of a new one.
    const std::vector<std::string> order = {"a", "a", "b", "b", "c", "c", "d", "d"};
    for (const char* path : {"a", "b", "c", "d"})
    {
        setStamp(path, 1);
    }
    WorkerOptions options;
    options.maxPaths = 1;
    SignatureWorker worker(fakeFunctions(), options);
    constexpr int SAMPLES = 5; // an image a sample, and one more
    for (int sample = 1; sample <= SAMPLES; ++sample)
    {
        worker.beginSample();
        for (const std::string& path : order)
        {
            (void) worker.find(path);
            EXPECT_LE(worker.pathCount(), 1U) << "sample " << sample;
        }
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    }
    for (const char* path : {"a", "b", "c", "d"})
    {
        EXPECT_NE(checksOf(path), 0) << path << " is checked within " << SAMPLES << " samples";
    }
}

TEST_F(SignatureWorkerTest, APathWaitingItsTurnThatNobodyAsksForAgainDoesNotHoldUpTheOthers)
{
    // #3403 review: the path admission resumes with, and the one passed over after it, both belong to
    // processes that have gone. A turn lasts at most until the end of the sample after the one its path was
    // last asked for in, whichever paths are still asked for, so a path asked for later is admitted.
    for (const char* path : {"a", "gone", "marker", "live"})
    {
        setStamp(path, 1);
    }
    setGate(false);
    WorkerOptions options;
    options.maxPaths = 1;
    SignatureWorker worker(fakeFunctions(), options);
    worker.beginSample();
    worker.request("a");
    EXPECT_FALSE(worker.find("gone").has_value()) << "turned away: the table is full of a, queued";
    EXPECT_FALSE(worker.find("marker").has_value());
    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));

    worker.beginSample();
    EXPECT_FALSE(worker.find("live").has_value()) << "the next sample: gone may still be asked for, and goes first";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(checksOf("live"), 0);

    worker.beginSample();
    EXPECT_FALSE(worker.find("live").has_value()) << "a whole sample without gone: its turn is over";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(checksOf("live"), 1);
    EXPECT_TRUE(worker.find("live").has_value());
    EXPECT_EQ(checksOf("gone"), 0);
    EXPECT_EQ(checksOf("marker"), 0);
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

TEST_F(SignatureWorkerTest, ACheckThatThrowsSomethingElseFailsTheWorkerWhichAnswersEveryPathRatherThanTerminating)
{
    // #3343: a std::exception from a check is answered Unknown (above); anything else escapes it, to
    // Domain::Worker's boundary. The worker ends Failed, never std::terminate, and says so: the path that
    // threw, the one queued behind it, and every path asked for later are answered Unknown with the reason,
    // so none stays "not checked yet" for good, the table drains, and nothing is checked again.
    for (const char* path : {"a", "b", "c"})
    {
        setStamp(path, 1);
    }
    fakes().throwNonException = true;
    SignatureWorker worker(fakeFunctions(), {});
    {
        const MeteredChecks metered;
        worker.request("a");
        ASSERT_TRUE(waitForChecksEntered(1));
        worker.request("b"); // queued behind a, which is being checked
        allowChecks(1);      // a's check throws 42
        ASSERT_TRUE(waitUntil([&worker] { return worker.find("b").has_value(); })) << "b is answered, not left queued";
    }
    for (const char* path : {"a", "b"})
    {
        const std::optional<SignatureCheck> found = worker.find(path);
        ASSERT_TRUE(found.has_value()) << path;
        const SignatureCheck check = found.value_or(SignatureCheck{});
        EXPECT_EQ(check.signature, ImageSignature::Unknown) << path;
        EXPECT_TRUE(check.note.contains("Unknown error")) << path << ": " << check.note;
    }
    EXPECT_EQ(checksOf("a"), 1);
    EXPECT_EQ(checksOf("b"), 0) << "never checked: the worker ended before it";

    worker.request("c"); // after the failure: answered at once
    const std::optional<SignatureCheck> later = worker.find("c");
    ASSERT_TRUE(later.has_value()) << "a path asked for after the failure is answered at once";
    EXPECT_EQ(later.value_or(SignatureCheck{}).signature, ImageSignature::Unknown);
    EXPECT_EQ(checksOf("c"), 0);
    EXPECT_EQ(worker.queuedPathCount(), 0U);
    EXPECT_TRUE(worker.waitUntilIdle(0ms)) << "nothing is left queued or in progress";
}

TEST_F(SignatureWorkerTest, ABurstAskedForWhileACheckIsInProgressIsEachCheckedOnce)
{
    // #3343: the worker's own queue holds one drain of the path table at most; a path queued while one is
    // queued merges into it, and one queued while a drain runs queues the next. However the burst lands
    // against a drain in progress, none of its paths is lost and none is checked twice.
    constexpr int BURST = 32;
    std::vector<std::string> paths{"first.exe"};
    for (int i = 0; i < BURST; ++i)
    {
        paths.push_back("burst-" + std::to_string(i) + ".exe");
    }
    for (const std::string& path : paths)
    {
        setStamp(path, 1);
    }
    SignatureWorker worker(fakeFunctions(), {});
    {
        const MeteredChecks metered;
        worker.request(paths.front());
        ASSERT_TRUE(waitForChecksEntered(1)); // the drain is running, its check held
        for (std::size_t i = 1; i < paths.size(); ++i)
        {
            worker.request(paths[i]);
            worker.request(paths[i]); // a second process of the same image: still queued once
        }
        EXPECT_EQ(worker.queuedPathCount(), paths.size()) << "the burst, and the path being checked";
    }
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    for (const std::string& path : paths)
    {
        EXPECT_EQ(checksOf(path), 1) << path;
        EXPECT_TRUE(worker.find(path).has_value()) << path;
    }
    EXPECT_EQ(worker.queuedPathCount(), 0U);
}

TEST_F(SignatureWorkerTest, APathWhoseDrainIsRefusedIsTakenBackAndCheckedWhenAskedAgain)
{
    // #3343: a drain the Worker can't queue (no memory) while none is queued leaves nothing sure to check
    // the paths queued. They are taken back, so a later sample's find() queues them again, rather than
    // leaving them "not checked yet" for good.
    setStamp("a", 1);
    SignatureWorker worker(fakeFunctions(), {});
    worker.refuseNextDrainForTesting();
    worker.request("a");
    EXPECT_EQ(worker.queuedPathCount(), 0U) << "taken back";
    EXPECT_EQ(worker.pathCount(), 0U) << "and forgotten, so it is asked for again";
    EXPECT_TRUE(worker.waitUntilIdle(0ms)) << "nothing is left queued";
    EXPECT_FALSE(worker.find("a").has_value()) << "asked for again: queued, with a drain this time";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find("a").has_value());
    EXPECT_EQ(checksOf("a"), 1);
}

TEST_F(SignatureWorkerTest, TheTakeBackAfterARefusedDrainAllocatesNoKeyForALongPath)
{
    // #3479 review: a drain is refused because memory has run out, so taking the queued paths back must not
    // need any. The seam has every key made under the lock throw std::bad_alloc during the take-back; paths
    // longer than any small-string buffer would need a heap key. The take-back keys by what admission made,
    // so request() doesn't throw and nothing is left queued with no drain to check it.
    const std::string first = "C:\\" + std::string(300, 'a') + ".exe";
    const std::string second = "C:\\" + std::string(300, 'b') + ".exe";
    setStamp(first, 1);
    setStamp(second, 1);
    setGate(false); // first's check is held, so second is queued behind it when its drain is refused
    SignatureWorker worker(fakeFunctions(), {});
    worker.request(first);
    worker.refuseNextDrainForTesting();
    EXPECT_NO_THROW(worker.request(second)) << "the take-back makes no key";
    EXPECT_LE(worker.queuedPathCount(), 1U) << "second, at least, taken back; first may be in its check";
    setGate(true);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.queuedPathCount(), 0U) << "nothing left queued with no drain to check it";

    // Both asked for again on a later sample (a path taken back is forgotten), and both checked.
    worker.beginSample();
    static_cast<void>(worker.find(first));
    static_cast<void>(worker.find(second));
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_TRUE(worker.find(first).has_value());
    EXPECT_TRUE(worker.find(second).has_value());
    EXPECT_EQ(checksOf(first), 1);
    EXPECT_EQ(checksOf(second), 1);
}

TEST_F(SignatureWorkerTest, TheDestructorWaitsForTheCheckInProgressAndDropsTheRest)
{
    // WinVerifyTrust can't be interrupted: shutdown while a check is in progress lets it finish, filing its
    // answer in a table still alive, and drops the paths queued behind it unchecked. An hour's pause after
    // each check means the worker can't reach two's check whenever the stop lands: if the check is let go
    // before the destructor asks to stop, the worker is in the pause, which the stop ends.
    for (const char* path : {"one", "two", "three"})
    {
        setStamp(path, 1);
    }
    WorkerOptions options;
    options.pause = std::chrono::hours(1);
    auto worker = std::make_unique<SignatureWorker>(fakeFunctions(), options);
    // After the worker, so it goes first: an assertion that ends the test lets the held check go before the
    // worker is destroyed (TearDown would be too late).
    const MeteredChecks metered;
    worker->request("one");
    worker->request("two");
    worker->request("three");
    ASSERT_TRUE(waitForChecksEntered(1)) << "one's check is in progress";
    std::atomic<bool> destroyed{false};
    std::thread destroying(
        [&worker, &destroyed]
        {
            worker.reset();
            destroyed = true;
        });
    // A negative check: it can't fail when the destructor waits, only miss one that doesn't.
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(destroyed.load()) << "the destructor waits for the check in progress";
    allowChecks(1);
    destroying.join();
    EXPECT_TRUE(destroyed.load());
    EXPECT_EQ(checksCompleted(), 1U) << "the check in progress finished";
    EXPECT_EQ(checksOf("one"), 1);
    EXPECT_EQ(checksOf("two"), 0) << "dropped";
    EXPECT_EQ(checksOf("three"), 0) << "dropped";
}

} // namespace
} // namespace Platform::WindowsSignature
