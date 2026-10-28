/// @file test_WindowsSignatureWorker.cpp
/// @brief The process probe's signature worker (#3070) through fake checks: a request is answered on a
/// later find(), never by the caller's thread; a path is queued once however many processes run it; a
/// new process of an image re-reads its stamp, checked again only when the file changed; the path table's
/// bound; a check that throws; and the destructor stopping a queue. Pure (no Windows header), so built
/// everywhere.

#include "Platform/ImageSignature.h"
#include "Platform/Windows/WindowsImageSignature.h"
#include "Platform/Windows/WindowsSignatureWorker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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
    fakes().started = false;
}

SignatureFunctions fakeFunctions()
{
    return {
        .errorMessage = nullptr,
        .fileStamp = [](const std::string& path) -> std::optional<FileStamp>
        {
            const std::scoped_lock lock(fakes().mutex);
            const auto found = fakes().stamps.find(path);
            return found != fakes().stamps.end() ? std::optional<FileStamp>(found->second) : std::nullopt;
        },
        .verifyEmbeddedSignature =
            [](const std::string& path)
        {
            std::unique_lock lock(fakes().mutex);
            fakes().gateChanged.wait(lock, [] { return fakes().gateOpen; });
            ++fakes().checks[path];
            fakes().checkThread = std::this_thread::get_id();
            if (fakes().throwOnCheck)
            {
                throw std::runtime_error("the check threw");
            }
            return TrustCheck{.result = 0, .signer = "Signer of " + path};
        },
        .verifyCatalogSignature = [](const std::string& /*path*/) -> std::optional<TrustCheck> { return std::nullopt; },
    };
}

void setStamp(const std::string& path, std::uint64_t lastWrite)
{
    const std::scoped_lock lock(fakes().mutex);
    fakes().stamps[path] = FileStamp{.lastWriteTime = lastWrite, .size = 100};
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
