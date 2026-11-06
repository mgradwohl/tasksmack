/// @file test_ProcessIconCache.cpp
/// @brief Domain::ProcessIconCache (#1800): a process's icon is loaded once, off the caller's thread,
/// shared by every process running the same executable, remembered as missing when there is none,
/// and held in bounded maps. Driven through a fake icon source, mostly without the worker
/// (loadPending() runs the queued loads on the test's thread).

#include "Domain/ProcessIconCache.h"
#include "Platform/IProcessIconSource.h"

#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Domain
{
namespace
{

/// What loadIcon() throws on the worker's thread (#3343): nothing, a std::exception, or something that
/// isn't one. Atomic, unlike throwOnLoad, since the test changes it while the worker runs.
enum class LoadThrows : std::uint8_t
{
    Nothing,
    StdException,
    NonStdException,
};

/// What the fake source was asked, and what it answers.
struct FakeIconSourceState
{
    std::map<std::int32_t, std::string> paths; ///< By PID; absent: unreadable
    std::map<std::string, int> iconSides;      ///< By path (or "?name" when no path): a solid icon this wide
    std::atomic<int> pathReads{0};
    std::atomic<int> iconReads{0};
    std::vector<int> requestedSizes;
    bool caseInsensitive = false;
    bool byName = false; ///< Answers by name when the path has no icon, as Linux's desktop entries do
    bool throwOnLoad = false;
    std::atomic<LoadThrows> loadThrows{LoadThrows::Nothing};
};

class FakeIconSource final : public Platform::IProcessIconSource
{
  public:
    explicit FakeIconSource(std::shared_ptr<FakeIconSourceState> state) : m_State(std::move(state))
    {}

    [[nodiscard]] std::string executablePath(std::int32_t pid) override
    {
        ++m_State->pathReads;
        const auto it = m_State->paths.find(pid);
        return it == m_State->paths.end() ? std::string{} : it->second;
    }

    [[nodiscard]] std::optional<Platform::IconPixels>
    loadIcon(std::string_view executablePath, std::string_view processName, int sizePx) override
    {
        ++m_State->iconReads;
        m_State->requestedSizes.push_back(sizePx);
        if (m_State->throwOnLoad)
        {
            throw std::runtime_error("unreadable");
        }
        switch (m_State->loadThrows.load())
        {
        case LoadThrows::StdException:
            throw std::runtime_error("unreadable");
        case LoadThrows::NonStdException:
            // NOLINTNEXTLINE(bugprone-std-exception-baseclass,hicpp-exception-baseclass) - the point: what a catch (std::exception) misses
            throw 42;
        case LoadThrows::Nothing:
            break;
        }
        const std::string key = executablePath.empty() ? "?" + std::string(processName) : std::string(executablePath);
        auto it = m_State->iconSides.find(key);
        if (it == m_State->iconSides.end() && m_State->byName)
        {
            it = m_State->iconSides.find("?" + std::string(processName));
        }
        if (it == m_State->iconSides.end())
        {
            return std::nullopt;
        }
        const int side = it->second;
        Platform::IconPixels pixels{.width = side, .height = side, .rgba = {}};
        pixels.rgba.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U, 200U);
        return pixels;
    }

    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return m_State->caseInsensitive;
    }

    [[nodiscard]] bool findsIconsByName() const noexcept override
    {
        return m_State->byName;
    }

  private:
    std::shared_ptr<FakeIconSourceState> m_State;
};

constexpr int SIZE = 16;

class ProcessIconCacheTest : public ::testing::Test
{
  protected:
    [[nodiscard]] std::unique_ptr<ProcessIconCache> makeCache(ProcessIconCacheConfig config = {.startWorker = false})
    {
        return std::make_unique<ProcessIconCache>(std::make_unique<FakeIconSource>(m_State), config);
    }

    std::shared_ptr<FakeIconSourceState> m_State = std::make_shared<FakeIconSourceState>();
};

TEST_F(ProcessIconCacheTest, TheFirstLookupQueuesALoadAndALaterOneReturnsTheIcon)
{
    m_State->paths[10] = "/usr/bin/app";
    m_State->iconSides["/usr/bin/app"] = SIZE;
    const auto cache = makeCache();

    EXPECT_EQ(cache->lookup(1, 10, "app", SIZE), nullptr); // Loading: the generic icon meanwhile
    EXPECT_EQ(cache->stats().queued, 1U);
    EXPECT_EQ(cache->lookup(1, 10, "app", SIZE), nullptr); // Still one load queued, not two
    EXPECT_EQ(cache->stats().queued, 1U);

    EXPECT_EQ(cache->loadPending(), 1U);
    const auto icon = cache->lookup(1, 10, "app", SIZE);
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->sizePx, SIZE);
    EXPECT_EQ(icon->rgba.size(), static_cast<std::size_t>(SIZE * SIZE * 4));
    EXPECT_NE(icon->id, 0U);
    EXPECT_EQ(m_State->iconReads.load(), 1);
}

TEST_F(ProcessIconCacheTest, ProcessesRunningOneExecutableShareOneIconAndOneRead)
{
    m_State->paths[10] = "C:\\Apps\\Browser.exe";
    m_State->paths[11] = "c:\\apps\\browser.EXE"; // Another case: the same file on Windows
    m_State->iconSides["C:\\Apps\\Browser.exe"] = SIZE;
    m_State->iconSides["c:\\apps\\browser.EXE"] = SIZE; // Either spelling reads the same file
    m_State->caseInsensitive = true;
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(1, 10, "Browser.exe", SIZE));
    static_cast<void>(cache->lookup(2, 11, "browser.exe", SIZE));
    EXPECT_EQ(cache->loadPending(), 2U);
    const auto first = cache->lookup(1, 10, "Browser.exe", SIZE);
    const auto second = cache->lookup(2, 11, "browser.exe", SIZE);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(m_State->iconReads.load(), 1);
    EXPECT_EQ(cache->stats().icons, 1U);
}

TEST_F(ProcessIconCacheTest, ACaseInsensitiveKeyFoldsAsciiLettersOnly)
{
    // The folded key joins ASCII spellings of one path but never a byte of 0x80 and up, whatever the
    // locale (#2566): 0xC4 (Latin-1's Ä) and 0xE4 (ä) are two files, so two reads and two icons.
    m_State->paths[10] = "C:\\Apps\\\xC4pp.exe";
    m_State->paths[11] = "c:\\APPS\\\xC4PP.EXE";
    m_State->paths[12] = "C:\\Apps\\\xE4pp.exe";
    m_State->iconSides["C:\\Apps\\\xC4pp.exe"] = SIZE;
    m_State->iconSides["c:\\APPS\\\xC4PP.EXE"] = SIZE; // Either spelling reads the same file
    m_State->iconSides["C:\\Apps\\\xE4pp.exe"] = SIZE;
    m_State->caseInsensitive = true;
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(1, 10, "\xC4pp.exe", SIZE));
    static_cast<void>(cache->lookup(2, 11, "\xC4PP.EXE", SIZE));
    static_cast<void>(cache->lookup(3, 12, "\xE4pp.exe", SIZE));
    EXPECT_EQ(cache->loadPending(), 3U);
    const auto upper = cache->lookup(1, 10, "\xC4pp.exe", SIZE);
    const auto upperAgain = cache->lookup(2, 11, "\xC4PP.EXE", SIZE);
    const auto lower = cache->lookup(3, 12, "\xE4pp.exe", SIZE);
    ASSERT_NE(upper, nullptr);
    ASSERT_NE(lower, nullptr);
    EXPECT_EQ(upper, upperAgain);
    EXPECT_NE(upper, lower);
    EXPECT_EQ(m_State->iconReads.load(), 2);
    EXPECT_EQ(cache->stats().icons, 2U);
}

TEST_F(ProcessIconCacheTest, PathsKeepTheirCaseWhereThePlatformDoes)
{
    m_State->paths[10] = "/opt/App";
    m_State->paths[11] = "/opt/app";
    m_State->iconSides["/opt/App"] = SIZE;
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(1, 10, "App", SIZE));
    static_cast<void>(cache->lookup(2, 11, "app", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(1, 10, "App", SIZE), nullptr);
    EXPECT_EQ(cache->lookup(2, 11, "app", SIZE), nullptr);
    EXPECT_EQ(m_State->iconReads.load(), 2);
}

TEST_F(ProcessIconCacheTest, AnExecutableWithoutAnIconIsRememberedAndNotReadAgain)
{
    m_State->paths[10] = "/usr/sbin/daemon";
    m_State->paths[11] = "/usr/sbin/daemon";
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(1, 10, "daemon", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->lookup(1, 10, "daemon", SIZE), nullptr);
    EXPECT_EQ(cache->stats().misses, 1U);
    EXPECT_EQ(cache->stats().queued, 0U); // Known to have none: not queued again

    // A second process of the same executable: its path is read, its icon is not.
    static_cast<void>(cache->lookup(2, 11, "daemon", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(m_State->iconReads.load(), 1);
    EXPECT_EQ(m_State->pathReads.load(), 2);
}

// #2462: Linux finds a script's icon by its process name, so processes sharing an interpreter don't share
// one icon (or one miss) by their executable.
TEST_F(ProcessIconCacheTest, AByNameSourceKeepsEachNamesIconForOneExecutable)
{
    m_State->byName = true;
    m_State->paths[10] = "/usr/bin/python3.12";
    m_State->paths[11] = "/usr/bin/python3.12";
    m_State->paths[12] = "/usr/bin/python3.12";
    m_State->iconSides["?meld"] = SIZE;
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(1, 10, "meld", SIZE));
    static_cast<void>(cache->lookup(2, 11, "backup-script", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(1, 10, "meld", SIZE), nullptr);
    EXPECT_EQ(cache->lookup(2, 11, "backup-script", SIZE), nullptr) << "not Meld's icon";
    EXPECT_EQ(m_State->iconReads.load(), 2);

    // Another Meld: the same path and name, so its icon is not read again.
    static_cast<void>(cache->lookup(3, 12, "meld", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(3, 12, "meld", SIZE), nullptr);
    EXPECT_EQ(m_State->iconReads.load(), 2);
}

// #2496: a process that execs keeps its PID and start time, but its icon is the new program's: first seen as
// its launcher script ("sh", no icon), it's looked up again once it's firefox.
TEST_F(ProcessIconCacheTest, AProcessThatExecsAnotherProgramGetsThatProgramsIcon)
{
    m_State->iconSides["?firefox"] = SIZE; // by name, as when its executable can't be read
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(1, 10, "sh", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->lookup(1, 10, "sh", SIZE), nullptr);

    static_cast<void>(cache->lookup(1, 10, "firefox", SIZE)); // the same process, after exec
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(1, 10, "firefox", SIZE), nullptr);
    EXPECT_EQ(m_State->iconReads.load(), 2);
}

TEST_F(ProcessIconCacheTest, WithoutAPathTheIconIsLookedUpByName)
{
    m_State->iconSides["?firefox"] = SIZE; // Unreadable /proc/[pid]/exe: the desktop entry by name
    const auto cache = makeCache();
    static_cast<void>(cache->lookup(1, 10, "firefox", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(1, 10, "firefox", SIZE), nullptr);
}

TEST_F(ProcessIconCacheTest, AReusedPidIsANewProcess)
{
    m_State->paths[10] = "/usr/bin/a";
    m_State->iconSides["/usr/bin/a"] = SIZE;
    const auto cache = makeCache();
    static_cast<void>(cache->lookup(1, 10, "a", SIZE));
    static_cast<void>(cache->loadPending());
    ASSERT_NE(cache->lookup(1, 10, "a", SIZE), nullptr);

    // PID 10 again, with another start time and so another uniqueKey, now running b.
    m_State->paths[10] = "/usr/bin/b";
    EXPECT_EQ(cache->lookup(2, 10, "b", SIZE), nullptr);
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->lookup(2, 10, "b", SIZE), nullptr); // b has no icon; a's isn't reused for it
}

TEST_F(ProcessIconCacheTest, ANewSizeLoadsAgainAtThatSize)
{
    m_State->paths[10] = "/usr/bin/app";
    m_State->iconSides["/usr/bin/app"] = 32;
    const auto cache = makeCache();
    static_cast<void>(cache->lookup(1, 10, "app", SIZE));
    static_cast<void>(cache->loadPending());
    ASSERT_NE(cache->lookup(1, 10, "app", SIZE), nullptr);

    EXPECT_EQ(cache->lookup(1, 10, "app", 24), nullptr); // The display scale changed
    static_cast<void>(cache->loadPending());
    const auto larger = cache->lookup(1, 10, "app", 24);
    ASSERT_NE(larger, nullptr);
    EXPECT_EQ(larger->sizePx, 24);
    EXPECT_EQ(larger->rgba.size(), static_cast<std::size_t>(24 * 24 * 4));
    EXPECT_EQ(m_State->requestedSizes, (std::vector<int>{16, 24}));
}

TEST_F(ProcessIconCacheTest, AnOddSizeIsReadAtTheNextStandardSizeAndScaledDown)
{
    m_State->paths[10] = "/usr/bin/app";
    m_State->iconSides["/usr/bin/app"] = 16;
    const auto cache = makeCache();
    static_cast<void>(cache->lookup(1, 10, "app", 13));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(m_State->requestedSizes, (std::vector<int>{16}));
    const auto icon = cache->lookup(1, 10, "app", 13);
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->rgba.size(), static_cast<std::size_t>(13 * 13 * 4));
}

TEST_F(ProcessIconCacheTest, TheIconMapIsBoundedAndAnEvictedIconLivesOnInItsProcess)
{
    const auto cache = makeCache({.maxIcons = 2, .maxMisses = 8, .maxProcesses = 16, .maxQueued = 16, .startWorker = false});
    for (std::int32_t pid = 1; pid <= 3; ++pid)
    {
        const std::string path = "/bin/p" + std::to_string(pid);
        m_State->paths[pid] = path;
        m_State->iconSides[path] = SIZE;
        static_cast<void>(cache->lookup(static_cast<std::uint64_t>(pid), pid, "p", SIZE));
    }
    static_cast<void>(cache->loadPending());
    const auto stats = cache->stats();
    EXPECT_EQ(stats.icons, 2U);
    EXPECT_EQ(stats.pixelBytes, static_cast<std::size_t>(2 * SIZE * SIZE * 4));
    // Every process still has its icon, the evicted one included.
    EXPECT_NE(cache->lookup(1, 1, "p", SIZE), nullptr);
    EXPECT_NE(cache->lookup(2, 2, "p", SIZE), nullptr);
    EXPECT_NE(cache->lookup(3, 3, "p", SIZE), nullptr);
}

TEST_F(ProcessIconCacheTest, TheMissMapAndTheProcessMapAreBounded)
{
    const auto cache = makeCache({.maxIcons = 4, .maxMisses = 2, .maxProcesses = 3, .maxQueued = 16, .startWorker = false});
    for (std::int32_t pid = 1; pid <= 5; ++pid)
    {
        m_State->paths[pid] = "/bin/none" + std::to_string(pid);
        static_cast<void>(cache->lookup(static_cast<std::uint64_t>(pid), pid, "none", SIZE));
    }
    static_cast<void>(cache->loadPending());
    const auto stats = cache->stats();
    EXPECT_EQ(stats.misses, 2U);
    EXPECT_EQ(stats.processes, 3U);
}

TEST_F(ProcessIconCacheTest, AFullQueueDropsItsOldestRequestWhichIsAskedAgainLater)
{
    m_State->paths[1] = "/bin/a";
    m_State->iconSides["/bin/a"] = SIZE;
    const auto cache = makeCache({.maxIcons = 4, .maxMisses = 4, .maxProcesses = 16, .maxQueued = 2, .startWorker = false});
    static_cast<void>(cache->lookup(1, 1, "a", SIZE));
    static_cast<void>(cache->lookup(2, 2, "b", SIZE));
    static_cast<void>(cache->lookup(3, 3, "c", SIZE)); // Drops process 1's request
    EXPECT_EQ(cache->stats().queued, 2U);
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr); // Forgotten, so queued again now
    EXPECT_EQ(cache->stats().queued, 1U);
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(1, 1, "a", SIZE), nullptr);
}

TEST_F(ProcessIconCacheTest, AThrowingSourceLeavesTheGenericIconAndTriesAgainLater)
{
    m_State->paths[1] = "/bin/a";
    m_State->iconSides["/bin/a"] = SIZE;
    m_State->throwOnLoad = true;
    const auto cache = makeCache();
    static_cast<void>(cache->lookup(1, 1, "a", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->stats().misses, 0U); // Not remembered as missing

    m_State->throwOnLoad = false;
    EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr); // Queued again
    static_cast<void>(cache->loadPending());
    EXPECT_NE(cache->lookup(1, 1, "a", SIZE), nullptr);
}

TEST_F(ProcessIconCacheTest, WithoutASourceEveryProcessHasNoIcon)
{
    ProcessIconCache cache(nullptr, {.startWorker = false});
    EXPECT_EQ(cache.lookup(1, 1, "a", SIZE), nullptr);
    EXPECT_EQ(cache.stats().queued, 0U);
}

TEST_F(ProcessIconCacheTest, TheWorkerLoadsWithoutTheCallersHelp)
{
    m_State->paths[10] = "/usr/bin/app";
    m_State->iconSides["/usr/bin/app"] = SIZE;
    auto cache = makeCache(ProcessIconCacheConfig{});
    EXPECT_EQ(cache->lookup(1, 10, "app", SIZE), nullptr);
    std::shared_ptr<const ProcessIconImage> icon;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (icon == nullptr && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        icon = cache->lookup(1, 10, "app", SIZE);
    }
    EXPECT_NE(icon, nullptr);
    cache.reset(); // Joins the worker
}

/// Looks process @p key up until it has an icon, or 10 s pass; the icon, or nullptr.
std::shared_ptr<const ProcessIconImage>
lookupUntilLoaded(ProcessIconCache& cache, std::uint64_t key, std::int32_t pid, std::string_view name)
{
    std::shared_ptr<const ProcessIconImage> icon = cache.lookup(key, pid, name, SIZE);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (icon == nullptr && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        icon = cache.lookup(key, pid, name, SIZE);
    }
    return icon;
}

TEST_F(ProcessIconCacheTest, TheWorkerLoadsEveryRowOfABurst)
{
    // #3343: the worker's job is one drain of the cache's queue, one queued at most, so a burst of rows
    // merges into one or two drains. None of the burst's loads is lost: a load queued while a drain runs
    // starts another.
    constexpr std::int32_t ROWS = 64;
    for (std::int32_t pid = 1; pid <= ROWS; ++pid)
    {
        m_State->paths[pid] = "/usr/bin/app" + std::to_string(pid);
        m_State->iconSides[m_State->paths[pid]] = SIZE;
    }
    auto cache = makeCache(ProcessIconCacheConfig{});
    for (std::int32_t pid = 1; pid <= ROWS; ++pid)
    {
        static_cast<void>(cache->lookup(static_cast<std::uint64_t>(pid), pid, "app", SIZE));
    }
    for (std::int32_t pid = 1; pid <= ROWS; ++pid)
    {
        EXPECT_NE(lookupUntilLoaded(*cache, static_cast<std::uint64_t>(pid), pid, "app"), nullptr) << "PID " << pid;
    }
    EXPECT_EQ(m_State->iconReads.load(), ROWS) << "each row read once";
    cache.reset(); // Stops and joins the worker
}

/// Waits until @p done answers true, or 10 s pass; whether it did. A wait on an outcome the worker
/// leaves in the cache, not a sleep standing in for one.
template<typename Done> bool waitUntil(Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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

TEST_F(ProcessIconCacheTest, ASourceThatThrowsOnTheWorkerEndsNeitherTheWorkerNorTheProcess)
{
    // #3343: a read that throws on the worker's thread, a std::exception or anything else, is contained:
    // the row keeps the generic icon and is forgotten, so it is asked again, and the worker goes on
    // loading other rows. Each row is read alone, so each kind of throw is the one its read met.
    m_State->paths[1] = "/bin/a";
    m_State->paths[2] = "/bin/b";
    m_State->paths[3] = "/bin/c";
    m_State->iconSides["/bin/a"] = SIZE;
    m_State->iconSides["/bin/b"] = SIZE;
    m_State->iconSides["/bin/c"] = SIZE;
    auto cache = makeCache(ProcessIconCacheConfig{});

    m_State->loadThrows = LoadThrows::StdException;
    EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr);
    ASSERT_TRUE(waitUntil([&cache] { return cache->stats().processes == 0; })) << "the std::exception read forgot its row";
    EXPECT_EQ(m_State->iconReads.load(), 1);

    m_State->loadThrows = LoadThrows::NonStdException;
    EXPECT_EQ(cache->lookup(2, 2, "b", SIZE), nullptr);
    ASSERT_TRUE(waitUntil([&cache] { return cache->stats().processes == 0; })) << "the non-std read forgot its row";
    EXPECT_EQ(m_State->iconReads.load(), 2);

    m_State->loadThrows = LoadThrows::Nothing;
    EXPECT_NE(lookupUntilLoaded(*cache, 3, 3, "c"), nullptr) << "the worker went on";
    EXPECT_NE(lookupUntilLoaded(*cache, 1, 1, "a"), nullptr) << "asked again, and read";
    EXPECT_NE(lookupUntilLoaded(*cache, 2, 2, "b"), nullptr) << "asked again after a non-std exception, and read";
    EXPECT_EQ(m_State->iconReads.load(), 5) << "each throwing row read once more, the other once";
    EXPECT_EQ(cache->stats().misses, 0U) << "a read that threw isn't remembered as missing";
    cache.reset();
}

TEST_F(ProcessIconCacheTest, ALoadWhoseDrainIsRefusedIsAskedAgainAndLoads)
{
    // #3343: a drain refused (no memory to queue one) while the worker is idle leaves no drain to run the
    // load. The load is taken back, row entry and all, so the row's next lookup queues it again and it
    // loads, rather than the row keeping the generic icon for good.
    m_State->paths[1] = "/bin/a";
    m_State->iconSides["/bin/a"] = SIZE;
    auto cache = makeCache(ProcessIconCacheConfig{});

    cache->refuseNextDrainForTesting();
    EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr);
    const ProcessIconCache::Stats refused = cache->stats();
    EXPECT_EQ(refused.queued, 0U) << "the refused load was taken back";
    EXPECT_EQ(refused.processes, 0U) << "and the row with it, so its next lookup asks again";

    EXPECT_NE(lookupUntilLoaded(*cache, 1, 1, "a"), nullptr) << "asked again, and loaded";
    EXPECT_EQ(m_State->iconReads.load(), 1);
    cache.reset();
}

/// The default logger's messages, for as long as this lives. A mutex sink: the worker logs too.
class LogCapture
{
  public:
    LogCapture() : m_Previous(spdlog::default_logger()), m_Sink(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(LINES))
    {
        auto logger = std::make_shared<spdlog::logger>("icon-cache-test", m_Sink);
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("%v");
        spdlog::set_default_logger(std::move(logger));
    }
    // NOLINTNEXTLINE(bugprone-exception-escape) - only building the registry can throw, and the constructor did that
    ~LogCapture()
    {
        spdlog::set_default_logger(m_Previous);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    /// How many of the messages so far contain @p text.
    [[nodiscard]] std::size_t count(std::string_view text) const
    {
        const std::vector<std::string> lines = m_Sink->last_formatted();
        return static_cast<std::size_t>(std::ranges::count_if(lines, [text](const std::string& line) { return line.contains(text); }));
    }

  private:
    static constexpr std::size_t LINES = 128;
    std::shared_ptr<spdlog::logger> m_Previous;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> m_Sink;
};

TEST_F(ProcessIconCacheTest, AWorkerThatFailsAfterAdmittingADrainIsReportedOnceAndItsRowsKeepTheGenericIcon)
{
    // #3343: the drain was admitted (Queued), then threw before it loaded anything. Later lookups of
    // the same rows find their entries, so no submit() sees Closed; the failure is still logged, once, and
    // the rows keep the generic icon: Failed is terminal, and no thread is left to load them.
    constexpr std::string_view STOPPED = "icon loads have stopped (drain failed for testing)";
    m_State->paths[1] = "/bin/a";
    m_State->iconSides["/bin/a"] = SIZE;
    const LogCapture log;
    auto cache = makeCache(ProcessIconCacheConfig{});

    cache->failNextDrainForTesting();
    EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr);
    ASSERT_TRUE(waitUntil(
        [&cache, &log, STOPPED]
        {
            EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr);
            return log.count(STOPPED) > 0;
        }))
        << "a lookup of the same row reports the failure";
    for (int frame = 0; frame < 8; ++frame)
    {
        EXPECT_EQ(cache->lookup(1, 1, "a", SIZE), nullptr) << "the row keeps the generic icon";
    }
    EXPECT_EQ(log.count(STOPPED), 1U) << "reported once";
    EXPECT_EQ(m_State->iconReads.load(), 0);
    cache.reset();
}

// #1800: files asked for by path -- the Startup and Services tabs' programs, packaged apps' logos.

TEST_F(ProcessIconCacheTest, AFileIsLoadedByItsPathWithoutAProcess)
{
    m_State->iconSides["C:/Apps/Startup.exe"] = SIZE;
    const auto cache = makeCache();
    const std::uint64_t key = iconFileKey("C:/Apps/Startup.exe");

    EXPECT_EQ(cache->lookupFile(key, "C:/Apps/Startup.exe", "Startup", SIZE), nullptr); // Loading
    EXPECT_EQ(cache->lookupFile(key, "C:/Apps/Startup.exe", "Startup", SIZE), nullptr); // One load queued, not two
    EXPECT_EQ(cache->stats().queued, 1U);
    EXPECT_EQ(cache->loadPending(), 1U);

    const auto icon = cache->lookupFile(key, "C:/Apps/Startup.exe", "Startup", SIZE);
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->sizePx, SIZE);
    EXPECT_EQ(m_State->pathReads.load(), 0) << "a file has no process to read a path from";
    EXPECT_EQ(cache->stats().files, 1U);
    EXPECT_EQ(cache->stats().processes, 0U);
}

TEST_F(ProcessIconCacheTest, AFileAndARunningProcessOfItShareOneIconAndOneRead)
{
    m_State->paths[10] = "C:/Apps/Startup.exe";
    m_State->iconSides["C:/Apps/Startup.exe"] = SIZE;
    const auto cache = makeCache();
    const std::uint64_t key = iconFileKey("C:/Apps/Startup.exe");

    static_cast<void>(cache->lookup(1, 10, "Startup.exe", SIZE));
    static_cast<void>(cache->lookupFile(key, "C:/Apps/Startup.exe", "Startup", SIZE));
    EXPECT_EQ(cache->loadPending(), 2U);

    const auto fromProcess = cache->lookup(1, 10, "Startup.exe", SIZE);
    const auto fromFile = cache->lookupFile(key, "C:/Apps/Startup.exe", "Startup", SIZE);
    ASSERT_NE(fromProcess, nullptr);
    EXPECT_EQ(fromProcess, fromFile);
    EXPECT_EQ(m_State->iconReads.load(), 1);
}

TEST_F(ProcessIconCacheTest, AFileWithoutAnIconIsRememberedAndAnEmptyPathIsNeverRead)
{
    const auto cache = makeCache();
    const std::uint64_t key = iconFileKey("C:/Apps/NoIcon.exe");
    static_cast<void>(cache->lookupFile(key, "C:/Apps/NoIcon.exe", "NoIcon", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->lookupFile(key, "C:/Apps/NoIcon.exe", "NoIcon", SIZE), nullptr);
    EXPECT_EQ(cache->stats().misses, 1U);
    EXPECT_EQ(cache->stats().queued, 0U) << "answered from the files map, not queued again";

    EXPECT_EQ(cache->lookupFile(iconFileKey(""), "", "Unresolved", SIZE), nullptr);
    EXPECT_EQ(cache->stats().queued, 0U);
    EXPECT_EQ(cache->stats().files, 1U);
    EXPECT_EQ(m_State->iconReads.load(), 1);
}

TEST_F(ProcessIconCacheTest, AFileKeyAndAProcessKeyOfOneValueAreKeptApart)
{
    m_State->paths[10] = "/usr/bin/process";
    m_State->iconSides["/usr/bin/process"] = SIZE;
    m_State->iconSides["/opt/file"] = SIZE * 2;
    const auto cache = makeCache();

    static_cast<void>(cache->lookup(7, 10, "process", SIZE));
    static_cast<void>(cache->lookupFile(7, "/opt/file", "file", SIZE));
    static_cast<void>(cache->loadPending());
    const auto process = cache->lookup(7, 10, "process", SIZE);
    const auto file = cache->lookupFile(7, "/opt/file", "file", SIZE);
    ASSERT_NE(process, nullptr);
    ASSERT_NE(file, nullptr);
    EXPECT_NE(process->id, file->id);
}

TEST_F(ProcessIconCacheTest, TheFilesMapIsBoundedAndAFullQueueDropsAFileToo)
{
    const auto cache = makeCache({.maxFiles = 2, .maxQueued = 1, .startWorker = false});
    static_cast<void>(cache->lookupFile(1, "/a", "a", SIZE));
    static_cast<void>(cache->lookupFile(2, "/b", "b", SIZE)); // Drops /a's request, and /a with it
    EXPECT_EQ(cache->stats().queued, 1U);
    EXPECT_EQ(cache->stats().files, 1U);
    static_cast<void>(cache->lookupFile(3, "/c", "c", SIZE));
    static_cast<void>(cache->lookupFile(4, "/d", "d", SIZE));
    EXPECT_EQ(cache->stats().files, 1U);
    static_cast<void>(cache->loadPending());
    static_cast<void>(cache->lookupFile(5, "/e", "e", SIZE));
    static_cast<void>(cache->lookupFile(6, "/f", "f", SIZE));
    EXPECT_LE(cache->stats().files, 2U);
}

TEST_F(ProcessIconCacheTest, AThrowingSourceLeavesAFileForAnotherTry)
{
    m_State->throwOnLoad = true;
    const auto cache = makeCache();
    static_cast<void>(cache->lookupFile(1, "/a", "a", SIZE));
    static_cast<void>(cache->loadPending());
    EXPECT_EQ(cache->stats().files, 0U) << "forgotten, so the next lookup asks again";
    static_cast<void>(cache->lookupFile(1, "/a", "a", SIZE));
    EXPECT_EQ(cache->stats().queued, 1U);
}

TEST(IconFileKeyTest, IsFnv1aOfThePath)
{
    EXPECT_EQ(iconFileKey(""), 14695981039346656037ULL);
    EXPECT_EQ(iconFileKey("a"), 0xAF63DC4C8601EC8CULL);
    EXPECT_NE(iconFileKey("C:/Apps/One.exe"), iconFileKey("C:/Apps/Two.exe"));
    static_assert(iconFileKey("x") == iconFileKey("x"));
}

TEST(ScaleIconToSquareTest, ShrinksByAveragingWithAlphaWeighting)
{
    // 2x2: one opaque red pixel, three transparent (black) ones -> one pixel: red, a quarter opaque.
    Platform::IconPixels pixels{.width = 2, .height = 2, .rgba = std::vector<std::uint8_t>(16, 0U)};
    pixels.rgba[0] = 255;
    pixels.rgba[3] = 255;
    const std::vector<std::uint8_t> out = scaleIconToSquare(pixels, 1);
    ASSERT_EQ(out.size(), 4U);
    EXPECT_EQ(out[0], 255); // Not darkened by the transparent pixels' black
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], 0);
    EXPECT_EQ(out[3], 63);
}

TEST(ScaleIconToSquareTest, GrowsByTheNearestPixelAndKeepsAnExactSize)
{
    const Platform::IconPixels pixels{.width = 1, .height = 1, .rgba = {1, 2, 3, 4}};
    EXPECT_EQ(scaleIconToSquare(pixels, 2), (std::vector<std::uint8_t>{1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4}));
    EXPECT_EQ(scaleIconToSquare(pixels, 1), pixels.rgba);
}

TEST(ScaleIconToSquareTest, RefusesPixelsThatDontMatchTheirSize)
{
    const Platform::IconPixels shortPixels{.width = 2, .height = 2, .rgba = {1, 2, 3}};
    EXPECT_TRUE(scaleIconToSquare(shortPixels, 2).empty());
    const Platform::IconPixels empty{};
    EXPECT_TRUE(scaleIconToSquare(empty, 2).empty());
}

TEST(IconSourceSizeTest, ReadsAtTheNextStandardSize)
{
    EXPECT_EQ(iconSourceSize(8), 16);
    EXPECT_EQ(iconSourceSize(16), 16);
    EXPECT_EQ(iconSourceSize(17), 20);
    EXPECT_EQ(iconSourceSize(33), 40);
    EXPECT_EQ(iconSourceSize(1000), 256);
}

} // namespace
} // namespace Domain
