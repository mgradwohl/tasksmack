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
/// drain in progress, each path checked once; a drain refused; and shutdown while a check is in progress. A process's
/// image bound to the file it was mapped from, never the decoy at its path (#4021). A process that has exited, or that
/// the probe no longer lists, dropped before its turn with no binding, check or pause (#4059).
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
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

/// The faults a test arms on a worker made with InjectedFaults (#3792); each fires once.
struct ArmedFaults
{
    std::atomic<bool> refuseNextDrain{false}; ///< The next drain submit() answers Refused
    std::atomic<bool> keysFail{false};        ///< The next key made under the lock throws (the take-back's)
    std::atomic<bool> endAnswerFails{false};  ///< The next answer made after the worker ended throws
};

/// The fault policy these tests put in place of NoSignatureWorkerFaults, the one the worker ships with
/// (#3792). A refused drain's take-back runs with every key made under the lock throwing std::bad_alloc, as
/// the memory that just ran out would: it must not need one.
struct InjectedFaults
{
    std::shared_ptr<ArmedFaults> armed = std::make_shared<ArmedFaults>();

    [[nodiscard]] bool refuseDrain() const
    {
        const bool refuse = armed->refuseNextDrain.exchange(false);
        if (refuse)
        {
            armed->keysFail = true;
        }
        return refuse;
    }
    void tookBack() const
    {
        armed->keysFail = false;
    }
    void makingKey() const
    {
        if (armed->keysFail.exchange(false))
        {
            throw std::bad_alloc();
        }
    }
    void makingEndAnswer() const
    {
        if (armed->endAnswerFails.exchange(false))
        {
            throw std::bad_alloc(); // the note's copy, out of memory
        }
    }
};

using FaultyWorker = BasicSignatureWorker<InjectedFaults>;

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
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Embedded);
    EXPECT_EQ(check.value_or(SignatureCheck{}).signer, "Signer of C:\\app.exe");
    EXPECT_TRUE(fakes().started.load());
    const std::scoped_lock lock(fakes().mutex);
    EXPECT_NE(fakes().checkThread, std::this_thread::get_id());
}

// #3703: the publisher is read with the check, on the worker's thread, and handed over with the answer.
TEST_F(SignatureWorkerTest, ThePublisherIsReadWithTheCheckOnTheWorkersThread)
{
    static std::thread::id publisherThread;
    publisherThread = {};
    setStamp("C:\\app.exe", 1);
    SignatureFunctions functions = fakeFunctions();
    functions.readPublisher = [](const std::string& path, void* /*file*/)
    {
        publisherThread = std::this_thread::get_id();
        return "Publisher of " + path;
    };
    SignatureWorker worker(functions, WorkerOptions{});
    std::string publisher = "untouched";
    EXPECT_FALSE(worker.find("C:\\app.exe", &publisher).has_value());
    EXPECT_EQ(publisher, "untouched") << "no answer yet: left alone";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    ASSERT_TRUE(worker.find("C:\\app.exe", &publisher).has_value());
    EXPECT_EQ(publisher, "Publisher of C:\\app.exe");
    EXPECT_NE(publisherThread, std::this_thread::get_id()) << "never on the asking (sampler) thread";
    EXPECT_NE(publisherThread, std::thread::id{});
}

TEST_F(SignatureWorkerTest, AnUnreadablePublisherIsNoneAndTheCheckStillAnswers)
{
    setStamp("C:\\app.exe", 1);
    SignatureFunctions functions = fakeFunctions();
    functions.readPublisher = [](const std::string& /*path*/, void* /*file*/) -> std::string
    {
        throw std::runtime_error("no version info");
    };
    SignatureWorker worker(functions, WorkerOptions{});
    (void) worker.find("C:\\app.exe");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    std::string publisher = "stale";
    const std::optional<SignatureCheck> check = worker.find("C:\\app.exe", &publisher);
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Embedded);
    EXPECT_TRUE(publisher.empty());

    SignatureWorker without(fakeFunctions(), WorkerOptions{}); // No publisher read at all
    (void) without.find("C:\\app.exe");
    ASSERT_TRUE(without.waitUntilIdle(WAIT));
    publisher = "stale";
    ASSERT_TRUE(without.find("C:\\app.exe", &publisher).has_value());
    EXPECT_TRUE(publisher.empty());
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
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Unknown);
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
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Unknown);
    EXPECT_EQ(check.value_or(SignatureCheck{}).note, "the check threw");
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

// #3862: an answer made because the worker ended read no publisher, so find() leaves the caller's alone, and
// ended() says the worker has ended, so the caller reads it itself. A path checked before the end keeps the one
// read with its check.
TEST_F(SignatureWorkerTest, AnAnswerMadeAfterTheWorkerEndedReadsNoPublisherAndEndedSaysSo)
{
    for (const char* path : {"a", "b", "c"})
    {
        setStamp(path, 1);
    }
    SignatureFunctions functions = fakeFunctions();
    functions.readPublisher = [](const std::string& path, void* /*file*/)
    {
        return "Publisher of " + path;
    };
    SignatureWorker worker(functions, {});
    worker.request("a");
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_FALSE(worker.ended());

    fakes().throwNonException = true;
    worker.request("b"); // its check throws 42: the worker fails
    ASSERT_TRUE(waitUntil([&worker] { return worker.ended(); })) << "the end is seen";
    std::string publisher = "untouched";
    ASSERT_TRUE(worker.find("b", &publisher).has_value());
    EXPECT_EQ(publisher, "untouched") << "the end answer read none";

    worker.request("c"); // a new process after the end: answered at once, with no publisher
    ASSERT_TRUE(worker.find("c", &publisher).has_value());
    EXPECT_EQ(publisher, "untouched");
    EXPECT_TRUE(worker.ended()) << "it stays ended";

    ASSERT_TRUE(worker.find("a", &publisher).has_value());
    EXPECT_EQ(publisher, "Publisher of a") << "checked before the end: read with its check";
}

TEST_F(SignatureWorkerTest, APathWhoseEndAnswerCouldNotBeMadeIsAnsweredByTheNextLookup)
{
    // #3479 review: once the worker has failed, a path asked for is answered at once with the end note. When
    // copying the note throws (no memory), the path is left queued; the next lookup answers it, even one of
    // that path alone (it is in the table, so asks for no new path that would sweep the queue), rather than
    // leaving it "not checked yet" for good.
    for (const char* path : {"a", "c"})
    {
        setStamp(path, 1);
    }
    fakes().throwNonException = true;
    const InjectedFaults faults;
    FaultyWorker worker(fakeFunctions(), {}, faults);
    worker.request("a"); // its check throws 42: the worker fails
    ASSERT_TRUE(waitUntil([&worker] { return worker.find("a").has_value(); })) << "the failure is seen";

    faults.armed->endAnswerFails = true;
    EXPECT_THROW(worker.request("c"), std::bad_alloc);
    EXPECT_EQ(worker.queuedPathCount(), 1U) << "c is left queued: its answer couldn't be made";
    EXPECT_FALSE(worker.waitUntilIdle(0ms));

    const std::optional<SignatureCheck> stranded = worker.find("c");
    ASSERT_TRUE(stranded.has_value()) << "the next lookup answers c";
    EXPECT_EQ(stranded.value_or(SignatureCheck{}).signature, ImageSignature::Unknown);
    EXPECT_TRUE(stranded.value_or(SignatureCheck{}).note.contains("Unknown error"));
    EXPECT_EQ(worker.queuedPathCount(), 0U);
    EXPECT_TRUE(worker.waitUntilIdle(0ms));
    EXPECT_EQ(checksOf("c"), 0);
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
    const InjectedFaults faults;
    FaultyWorker worker(fakeFunctions(), {}, faults);
    faults.armed->refuseNextDrain = true;
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
    const InjectedFaults faults;
    FaultyWorker worker(fakeFunctions(), {}, faults);
    worker.request(first);
    faults.armed->refuseNextDrain = true;
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

// --- A process's image bound to the file it was mapped from (#4021) ----------------------------------------------

/// What the fake openProcessImage() binds each PID to: the file's name now (a stamp set with setStamp()), or, for a
/// PID missing here, a refusal. Guarded by fakes().mutex.
std::map<std::int32_t, std::string>& boundFiles()
{
    static std::map<std::int32_t, std::string> files;
    return files;
}

/// Each process the fake openProcessImage() was asked to bind. Guarded by fakes().mutex.
std::vector<ImageProcess>& bindings()
{
    static std::vector<ImageProcess> asked;
    return asked;
}

/// The PIDs the fake processExited() says have exited (#4059). Guarded by fakes().mutex.
std::set<std::int32_t>& exitedPids()
{
    static std::set<std::int32_t> pids;
    return pids;
}

/// Each process the fake processExited() was asked about. Guarded by fakes().mutex.
std::vector<ImageProcess>& exitChecks()
{
    static std::vector<ImageProcess> asked;
    return asked;
}

/// The thread the fake processExited() was last asked on. Guarded by fakes().mutex.
std::thread::id& exitCheckThread()
{
    static std::thread::id thread;
    return thread;
}

constexpr std::string_view UNBOUND = "Its executable can't be found";
constexpr std::int32_t PROTECTED_PID = 50; ///< A protected process (csrss.exe, say): never bound

SignatureFunctions bindingFunctions()
{
    SignatureFunctions functions = fakeFunctions();
    functions.openProcessImage = [](const ImageProcess& process) -> std::expected<BoundImage, BindFailure>
    {
        const std::scoped_lock lock(fakes().mutex);
        bindings().push_back(process);
        const auto bound = boundFiles().find(process.pid);
        if (bound == boundFiles().end())
        {
            // PROTECTED_PID can never be bound, as a protected process's memory map isn't shown.
            return std::unexpected(BindFailure{.why = std::string(UNBOUND), .protectedProcess = process.pid == PROTECTED_PID});
        }
        const auto stamp = fakes().stamps.find(bound->second);
        if (stamp == fakes().stamps.end())
        {
            return std::unexpected(BindFailure{.why = "Its executable can't be read", .protectedProcess = false});
        }
        return BoundImage{.image = {.file = nullptr, .stamp = stamp->second}, .path = bound->second};
    };
    functions.readPublisher = [](const std::string& path, void* /*file*/)
    {
        return "Publisher of " + path;
    };
    functions.processExited = [](const ImageProcess& process)
    {
        const std::scoped_lock lock(fakes().mutex);
        exitChecks().push_back(process);
        exitCheckThread() = std::this_thread::get_id();
        return exitedPids().contains(process.pid);
    };
    return functions;
}

/// Binds @p pid to the file now named @p path.
void bindTo(std::int32_t pid, const std::string& path)
{
    const std::scoped_lock lock(fakes().mutex);
    boundFiles()[pid] = path;
}

class SignatureBindingTest : public SignatureWorkerTest
{
  protected:
    void SetUp() override
    {
        SignatureWorkerTest::SetUp();
        const std::scoped_lock lock(fakes().mutex);
        boundFiles().clear();
        bindings().clear();
        exitedPids().clear();
        exitChecks().clear();
        exitCheckThread() = {};
    }
};

// The issue's case: a process renamed its image and left a signed decoy at the old name. The check and the publisher
// are the file the image was mapped from, read through the binding's handle; the decoy at the path is never opened.
TEST_F(SignatureBindingTest, AProcessIsCheckedAsTheFileItsImageWasMappedFromNotTheFileAtItsPath)
{
    setStamp("C:\\app.exe", 1);     // the decoy, at the name the process started with
    setStamp("C:\\renamed.exe", 2); // the image's file, renamed
    bindTo(7, "C:\\renamed.exe");
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess process{.pid = 7, .startTimeTicks = 100};
    worker.request("C:\\app.exe", process);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    std::string publisher = "untouched";
    const std::optional<SignatureCheck> check = worker.find("C:\\app.exe", process, &publisher);
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Embedded);
    EXPECT_EQ(check.value_or(SignatureCheck{}).signer, "Signer of C:\\renamed.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\renamed.exe");
    EXPECT_EQ(checksOf("C:\\app.exe"), 0) << "the decoy is never checked";
    const std::scoped_lock lock(fakes().mutex);
    ASSERT_EQ(bindings().size(), 1U);
    EXPECT_EQ(bindings().front(), process) << "bound by the process's identity";
}

// A process whose file can't be bound reads Unknown, saying why, and no publisher: never the decoy's.
TEST_F(SignatureBindingTest, AProcessWhoseImageCantBeBoundIsUnknownWithNoPublisher)
{
    setStamp("C:\\app.exe", 1); // the decoy
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess process{.pid = 7, .startTimeTicks = 100};
    worker.request("C:\\app.exe", process);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    std::string publisher = "stale";
    const std::optional<SignatureCheck> check = worker.find("C:\\app.exe", process, &publisher);
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Unknown);
    const std::string note = check.value_or(SignatureCheck{}).note;
    EXPECT_EQ(note, std::string(UNBOUND) + "; its signature and publisher weren't read") << "said once, not twice";
    EXPECT_TRUE(check.value_or(SignatureCheck{}).signer.empty());
    EXPECT_TRUE(publisher.empty()) << "read with the answer: none, so the caller shows none";
    EXPECT_EQ(checksOf("C:\\app.exe"), 0) << "no fallback to the path";
}

// Each process of a path has its own answer, by its own binding: one renamed away from a decoy doesn't share the
// decoy's answer with a process that runs the decoy, nor a refused binding with either. A second process bound to
// the same file is answered from the cache, by the file's name and stamp: one WinVerifyTrust check a file.
TEST_F(SignatureBindingTest, EachProcessOfAPathHasItsOwnAnswerAndTheSameFileIsCheckedOnce)
{
    setStamp("C:\\app.exe", 1);
    setStamp("C:\\renamed.exe", 2);
    bindTo(7, "C:\\renamed.exe"); // renamed its image away
    bindTo(8, "C:\\app.exe");     // started from the file now at the path
    bindTo(9, "C:\\renamed.exe"); // another process of the renamed image
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess renamed{.pid = 7, .startTimeTicks = 100};
    const ImageProcess runsTheFileThere{.pid = 8, .startTimeTicks = 200};
    const ImageProcess unbound{.pid = 10, .startTimeTicks = 300};
    const ImageProcess sameImage{.pid = 9, .startTimeTicks = 400};
    for (const ImageProcess& process : {renamed, runsTheFileThere, unbound, sameImage})
    {
        worker.request("C:\\app.exe", process);
    }
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.pathCount(), 4U) << "one answer a process";

    std::string publisher;
    EXPECT_EQ(worker.find("C:\\app.exe", renamed, &publisher).value_or(SignatureCheck{}).signer, "Signer of C:\\renamed.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\renamed.exe");
    EXPECT_EQ(worker.find("C:\\app.exe", runsTheFileThere, &publisher).value_or(SignatureCheck{}).signer, "Signer of C:\\app.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\app.exe");
    EXPECT_EQ(worker.find("C:\\app.exe", unbound, &publisher).value_or(SignatureCheck{}).signature, ImageSignature::Unknown);
    EXPECT_TRUE(publisher.empty());
    EXPECT_EQ(worker.find("C:\\app.exe", sameImage, &publisher).value_or(SignatureCheck{}).signer, "Signer of C:\\renamed.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\renamed.exe") << "kept with the cached check";
    EXPECT_EQ(checksOf("C:\\renamed.exe"), 1) << "the second process of the file comes from the cache";
    EXPECT_EQ(checksOf("C:\\app.exe"), 1);
}

// #4276: the probe makes a process's key once and asks with it every sample. The answer is the one find() without a
// key gives, and a key the table doesn't hold yet requests its path, as find() does.
TEST_F(SignatureBindingTest, AFindWithTheCallersKeyAnswersAsAFindThatMakesItAndRequestsAnUnknownPath)
{
    setStamp("C:\\app.exe", 1);
    setStamp("C:\\renamed.exe", 2);
    bindTo(7, "C:\\renamed.exe");
    bindTo(8, "C:\\app.exe");
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess renamed{.pid = 7, .startTimeTicks = 100};
    const ImageProcess other{.pid = 8, .startTimeTicks = 200};
    const std::string renamedKey = SignatureWorker::keyFor("C:\\App.EXE", renamed);
    EXPECT_EQ(renamedKey, SignatureWorker::keyFor("c:\\app.exe", renamed)) << "the path's case is folded, as the table's is";
    EXPECT_NE(renamedKey, SignatureWorker::keyFor("C:\\app.exe", other)) << "each process has its own";

    worker.beginSample();
    EXPECT_FALSE(worker.find("C:\\app.exe", renamed, renamedKey).has_value()) << "not in the table: requested, not answered";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.pathCount(), 1U);

    std::string keyed;
    std::string made;
    const std::optional<SignatureCheck> withKey = worker.find("C:\\app.exe", renamed, renamedKey, &keyed);
    const std::optional<SignatureCheck> withoutKey = worker.find("C:\\app.exe", renamed, &made);
    ASSERT_TRUE(withKey.has_value());
    ASSERT_TRUE(withoutKey.has_value());
    EXPECT_EQ(withKey.value_or(SignatureCheck{}).signer, "Signer of C:\\renamed.exe");
    EXPECT_EQ(withKey.value_or(SignatureCheck{}).signer, withoutKey.value_or(SignatureCheck{}).signer);
    EXPECT_EQ(keyed, "Publisher of C:\\renamed.exe");
    EXPECT_EQ(keyed, made);
    EXPECT_EQ(worker.pathCount(), 1U) << "the same entry: no second request";
    EXPECT_EQ(checksOf("C:\\renamed.exe"), 1);
}

// #4280 review: an empty key (a caller whose key was never made) is never looked up or filed. Two processes asking
// with one can't share an entry: each is filed under, and answered from, the key find() makes for it.
TEST_F(SignatureBindingTest, AFindWithAnEmptyKeyIsAnsweredUnderTheKeyFindMakesNotUnderTheEmptyOne)
{
    setStamp("C:\\app.exe", 1);
    setStamp("C:\\renamed.exe", 2);
    bindTo(7, "C:\\renamed.exe");
    bindTo(8, "C:\\app.exe");
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess renamed{.pid = 7, .startTimeTicks = 100};
    const ImageProcess runsTheFileThere{.pid = 8, .startTimeTicks = 200};
    const std::string empty;
    worker.beginSample();
    EXPECT_FALSE(worker.find("C:\\app.exe", renamed, empty).has_value());
    EXPECT_FALSE(worker.find("C:\\app.exe", runsTheFileThere, empty).has_value());
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.pathCount(), 2U) << "one entry a process, none under the empty key";

    std::string publisher;
    EXPECT_EQ(worker.find("C:\\app.exe", renamed, empty, &publisher).value_or(SignatureCheck{}).signer, "Signer of C:\\renamed.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\renamed.exe");
    EXPECT_EQ(worker.find("C:\\app.exe", runsTheFileThere, empty, &publisher).value_or(SignatureCheck{}).signer, "Signer of C:\\app.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\app.exe") << "each process's own answer";
    EXPECT_EQ(worker.find("C:\\app.exe", renamed, SignatureWorker::keyFor("C:\\app.exe", renamed)).value_or(SignatureCheck{}).signer,
              "Signer of C:\\renamed.exe")
        << "the same entry the made key names";
    EXPECT_EQ(worker.pathCount(), 2U);
}

// The publisher the probe reads once the worker has ended (#3862) goes through the same binding: the bound file's,
// none when it can't be bound, and the path's only with no binding to make (no openProcessImage, or no process).
TEST_F(SignatureBindingTest, ThePublisherReadWithoutTheWorkerIsTheBoundFilesToo)
{
    setStamp("C:\\app.exe", 1);
    setStamp("C:\\renamed.exe", 2);
    bindTo(7, "C:\\renamed.exe");
    const SignatureFunctions functions = bindingFunctions();
    EXPECT_EQ(readImagePublisher(functions, "C:\\app.exe", ImageProcess{.pid = 7, .startTimeTicks = 1}), "Publisher of C:\\renamed.exe");
    EXPECT_TRUE(readImagePublisher(functions, "C:\\app.exe", ImageProcess{.pid = 8, .startTimeTicks = 1}).empty()) << "unbound: none";
    EXPECT_EQ(readImagePublisher(functions, "C:\\app.exe", std::nullopt), "Publisher of C:\\app.exe") << "no process: the path";

    SignatureFunctions pathOnly = functions;
    pathOnly.openProcessImage = nullptr;
    EXPECT_EQ(readImagePublisher(pathOnly, "C:\\app.exe", ImageProcess{.pid = 7, .startTimeTicks = 1}), "Publisher of C:\\app.exe");
    EXPECT_TRUE(readImagePublisher(pathOnly, "C:\\missing.exe", std::nullopt).empty()) << "can't be opened: none";
    pathOnly.readPublisher = nullptr;
    EXPECT_TRUE(readImagePublisher(pathOnly, "C:\\app.exe", std::nullopt).empty()) << "no publisher call";
}

// checkImage() itself: no calls is NotChecked, as checkSignature(); a binding is never tried without a process.
TEST_F(SignatureBindingTest, CheckImageWithoutCallsOrWithoutAProcess)
{
    EXPECT_EQ(checkImage(SignatureFunctions{}, nullptr, "C:\\app.exe", "c:\\app.exe", ImageProcess{}).check.signature,
              ImageSignature::NotChecked);
    setStamp("C:\\app.exe", 1);
    const ImageCheck byPath = checkImage(bindingFunctions(), nullptr, "C:\\app.exe", "c:\\app.exe", std::nullopt);
    EXPECT_EQ(byPath.check.signer, "Signer of C:\\app.exe");
    EXPECT_EQ(byPath.publisher, "Publisher of C:\\app.exe");
    const ImageCheck missing = checkImage(bindingFunctions(), nullptr, "C:\\missing.exe", "c:\\missing.exe", std::nullopt);
    EXPECT_EQ(missing.check.signature, ImageSignature::Unknown);
    EXPECT_TRUE(missing.publisher.empty());
    const std::scoped_lock lock(fakes().mutex);
    EXPECT_TRUE(bindings().empty());
}

// #4021 review: a protected process's image can never be bound (Windows doesn't show its memory map), but it could
// only start from a file with a protected signer: checked by its path, as before, not left Unknown for its lifetime.
TEST_F(SignatureBindingTest, AProtectedProcessThatCantBeBoundIsCheckedByItsPath)
{
    const std::string csrssPath = R"(C:\Windows\System32\csrss.exe)";
    setStamp(csrssPath, 1);
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess csrss{.pid = PROTECTED_PID, .startTimeTicks = 100};
    worker.request(csrssPath, csrss);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    std::string publisher;
    const std::optional<SignatureCheck> check = worker.find(csrssPath, csrss, &publisher);
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check.value_or(SignatureCheck{}).signature, ImageSignature::Embedded) << check.value_or(SignatureCheck{}).note;
    EXPECT_EQ(check.value_or(SignatureCheck{}).signer, "Signer of " + csrssPath);
    EXPECT_EQ(publisher, "Publisher of " + csrssPath);
    EXPECT_EQ(checksOf(csrssPath), 1);
    EXPECT_EQ(readImagePublisher(bindingFunctions(), csrssPath, csrss), "Publisher of " + csrssPath)
        << "the publisher fallback falls back the same way";
}

// #4021 review: the pause between files follows a WinVerifyTrust call only; a failed binding doesn't keep the next
// file waiting.
TEST_F(SignatureBindingTest, OnlyARealCheckIsPaused)
{
    setStamp("C:\\app.exe", 1);
    bindTo(8, "C:\\app.exe");
    WorkerOptions options;
    options.pause = std::chrono::hours(1);
    SignatureWorker worker(bindingFunctions(), options);
    worker.request("C:\\other.exe", ImageProcess{.pid = 9, .startTimeTicks = 1}); // unbound: no check, no pause
    worker.request("C:\\app.exe", ImageProcess{.pid = 8, .startTimeTicks = 1});   // checked, then the hour's pause
    ASSERT_TRUE(worker.waitUntilIdle(WAIT)) << "the failed binding didn't pause before the check";
    EXPECT_EQ(checksOf("C:\\app.exe"), 1);
}

// #4059: a build's short-lived processes queued ahead of a long-lived one have exited by their turn. Each is dropped
// with no binding, no check and no pause (an hour here: one of them checked would hold the live one for it), and leaves
// no answer and no cache entry; the live one, queued last, is answered. Bound to a signed file, as a zombie's could be
// (exited, its object held open by a parent that hasn't reaped it), each would be checked and paused for if it weren't
// dropped first.
TEST_F(SignatureBindingTest, ExitedProcessesQueuedAheadAreDroppedAndTheLiveOneIsAnsweredWithoutWaitingForThem)
{
    constexpr std::int32_t FIRST_EXITED = 100;
    constexpr std::int32_t EXITED_COUNT = 200;
    constexpr std::int32_t LIVE_PID = 1;
    setStamp("C:\\cl.exe", 1);
    setStamp("C:\\app.exe", 2);
    bindTo(LIVE_PID, "C:\\app.exe");
    {
        const std::scoped_lock lock(fakes().mutex);
        for (std::int32_t pid = FIRST_EXITED; pid < FIRST_EXITED + EXITED_COUNT; ++pid)
        {
            boundFiles()[pid] = "C:\\cl.exe";
            exitedPids().insert(pid);
        }
    }
    WorkerOptions options;
    options.pause = std::chrono::hours(1);
    SignatureWorker worker(bindingFunctions(), options);
    for (std::int32_t pid = FIRST_EXITED; pid < FIRST_EXITED + EXITED_COUNT; ++pid)
    {
        worker.request("C:\\cl.exe", ImageProcess{.pid = pid, .startTimeTicks = 1});
    }
    const ImageProcess live{.pid = LIVE_PID, .startTimeTicks = 1};
    worker.request("C:\\app.exe", live);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT)) << "no exited process was checked and paused for";
    EXPECT_EQ(worker.pathCount(), 1U) << "the exited ones leave no answer";
    EXPECT_EQ(worker.queuedPathCount(), 0U);
    EXPECT_EQ(worker.cachedFileCount(), 1U) << "nor a cache entry: only the live one's file";
    std::string publisher;
    const std::optional<SignatureCheck> check = worker.find("C:\\app.exe", live, &publisher);
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check.value_or(SignatureCheck{}).signer, "Signer of C:\\app.exe");
    EXPECT_EQ(publisher, "Publisher of C:\\app.exe");
    EXPECT_EQ(checksOf("C:\\cl.exe"), 0);
    const std::scoped_lock lock(fakes().mutex);
    ASSERT_EQ(bindings().size(), 1U) << "no exited process was bound";
    EXPECT_EQ(bindings().front(), live);
    EXPECT_EQ(exitChecks().size(), static_cast<std::size_t>(EXITED_COUNT) + 1) << "one exit check a process";
    EXPECT_EQ(exitCheckThread(), fakes().checkThread) << "asked on the worker's thread";
    EXPECT_NE(exitCheckThread(), std::this_thread::get_id()) << "never the caller's";
}

// #4059: a queued process the probe stopped asking for (no request() or find() through a whole sample) has left its
// list, and is dropped at its turn with no call at all, not even the exit check. One still asked for each sample is
// checked as usual, however long it waited.
TEST_F(SignatureBindingTest, AQueuedProcessTheProbeNoLongerListsIsDroppedWithoutAnExitCheck)
{
    setStamp("C:\\first.exe", 1);
    setStamp("C:\\gone.exe", 2);
    setStamp("C:\\listed.exe", 3);
    bindTo(1, "C:\\first.exe");
    bindTo(2, "C:\\gone.exe");
    bindTo(3, "C:\\listed.exe");
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const MeteredChecks metered; // after the worker: goes first, so the worker never joins on a held check
    const ImageProcess first{.pid = 1, .startTimeTicks = 1};
    const ImageProcess gone{.pid = 2, .startTimeTicks = 1};
    const ImageProcess listed{.pid = 3, .startTimeTicks = 1};
    worker.beginSample();
    worker.request("C:\\first.exe", first);
    ASSERT_TRUE(waitForChecksEntered(1)) << "the worker holds the first check while the others wait";
    worker.request("C:\\gone.exe", gone);
    worker.request("C:\\listed.exe", listed);
    worker.beginSample();
    EXPECT_FALSE(worker.find("C:\\listed.exe", listed).has_value()) << "still listed: asked for again, still queued";
    worker.beginSample(); // a whole sample without an ask for `gone`
    allowChecks(10);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.queuedPathCount(), 0U);
    EXPECT_EQ(worker.pathCount(), 2U) << "the unlisted one leaves no answer";
    EXPECT_EQ(worker.find("C:\\listed.exe", listed).value_or(SignatureCheck{}).signer, "Signer of C:\\listed.exe");
    EXPECT_EQ(checksOf("C:\\gone.exe"), 0);
    const std::scoped_lock lock(fakes().mutex);
    EXPECT_EQ(bindings().size(), 2U) << "the unlisted one wasn't bound";
    EXPECT_TRUE(std::ranges::none_of(bindings(), [&gone](const ImageProcess& asked) { return asked == gone; }));
    EXPECT_TRUE(std::ranges::none_of(exitChecks(), [&gone](const ImageProcess& asked) { return asked == gone; }))
        << "decided from the samples alone";
}

// #4059: an exited process dropped while the probe still lists it (a zombie, say) is simply requested again by the
// next find(), and dropped again: never answered, never left queued.
TEST_F(SignatureBindingTest, AnExitedProcessStillAskedForIsRequestedAgainAndDroppedAgain)
{
    setStamp("C:\\cl.exe", 1);
    bindTo(5, "C:\\cl.exe");
    {
        const std::scoped_lock lock(fakes().mutex);
        exitedPids().insert(5);
    }
    SignatureWorker worker(bindingFunctions(), WorkerOptions{});
    const ImageProcess zombie{.pid = 5, .startTimeTicks = 1};
    worker.request("C:\\cl.exe", zombie);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.pathCount(), 0U);
    EXPECT_FALSE(worker.find("C:\\cl.exe", zombie).has_value()) << "requested again";
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.pathCount(), 0U);
    EXPECT_EQ(worker.queuedPathCount(), 0U);
    const std::scoped_lock lock(fakes().mutex);
    EXPECT_TRUE(bindings().empty());
    EXPECT_EQ(exitChecks().size(), 2U);
}

} // namespace
} // namespace Platform::WindowsSignature
