/// @file test_FindHandlesFormat.cpp
/// @brief The Find handles dialog's pure parts (#3067, #3374): the results' order for every column and direction,
/// with its tie-breaks, every part of the status line, and a search stopped before it ran (#3343).

#include "App/Panels/FindHandlesDialog.h"
#include "Domain/HandleSearch.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace
{

using Domain::HandleSearch::HandleKind;
using Domain::HandleSearch::HandleMatch;

HandleMatch match(std::int32_t pid, std::string process, HandleKind kind, std::string path)
{
    return HandleMatch{.target = {.pid = pid, .startTimeTicks = 1000U + static_cast<std::uint64_t>(pid)},
                       .process = std::move(process),
                       .kind = kind,
                       .path = std::move(path),
                       .deleted = false};
}

/// Four matches whose order differs on every column.
std::vector<HandleMatch> matches()
{
    return {
        match(30, "nginx", HandleKind::Socket, "/run/b.sock"), // 0
        match(10, "Bash", HandleKind::File, "/home/a.txt"),    // 1
        match(20, "Zsh", HandleKind::Directory, "/Home/z"),    // 2
        match(40, "cron", HandleKind::Device, "/lib/c.so"),    // 3
    };
}

TEST(FindHandlesOrderTest, EachColumnSortsBothWays)
{
    const std::vector<HandleMatch> list = matches();
    using App::FindHandlesColumn;
    // Process names fold ASCII case: Bash, cron, nginx, Zsh (case-sensitive would put Zsh before cron).
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Process, true), (std::vector<std::size_t>{1, 3, 0, 2}));
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Process, false), (std::vector<std::size_t>{2, 0, 3, 1}));
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Pid, true), (std::vector<std::size_t>{1, 2, 0, 3}));
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Pid, false), (std::vector<std::size_t>{3, 0, 2, 1}));
    // By the label the table shows: Device, File, Folder, Socket. Device's enum value comes after the others, so
    // comparing enum values instead would put it last.
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Kind, true), (std::vector<std::size_t>{3, 1, 2, 0}));
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Kind, false), (std::vector<std::size_t>{0, 2, 1, 3}));
    // Paths fold ASCII case too: /home/a.txt, /Home/z, /lib/c.so, /run/b.sock.
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Path, true), (std::vector<std::size_t>{1, 2, 3, 0}));
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Path, false), (std::vector<std::size_t>{0, 3, 2, 1}));
}

TEST(FindHandlesOrderTest, TiesGoByPidThenPathAndPathsStayAToZEitherWay)
{
    // Three matches of one process kind: the column ties, so PID decides, and within one PID the path, A-Z
    // whichever way the column sorts.
    const std::vector<HandleMatch> list = {
        match(20, "app", HandleKind::File, "/b"), // 0
        match(10, "app", HandleKind::File, "/z"), // 1
        match(20, "app", HandleKind::File, "/a"), // 2
    };
    using App::FindHandlesColumn;
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Kind, true), (std::vector<std::size_t>{1, 2, 0}));
    EXPECT_EQ(App::orderFindHandlesMatches(list, FindHandlesColumn::Kind, false), (std::vector<std::size_t>{2, 0, 1}));
    EXPECT_TRUE(App::orderFindHandlesMatches({}, FindHandlesColumn::Process, true).empty());
}

TEST(FindHandlesStatusTest, EveryPartOfTheStatusLine)
{
    EXPECT_EQ(App::findHandlesStatus(Domain::HandleSearch::SearchPublication{}, false), "Type a name and press Find.");

    Domain::HandleSearch::SearchPublication done = Domain::HandleSearch::startFor("libssl", 12, 1);
    done.searched = 12;
    done.done = true;
    done.matches = matches();
    done.truncated = true;
    done.denied.resize(3);
    done.failed = 2;
    done.exited = 1;
    EXPECT_EQ(App::findHandlesStatus(done, true),
              "12 processes searched \xC2\xB7 4 found (the first ones) \xC2\xB7 3 couldn't be read \xC2\xB7 2 failed "
              "\xC2\xB7 1 ended first");
    // #3357: processes some of whose handles weren't named in time (Windows) come after those that ended.
    done.partlyNamed = 2;
    EXPECT_EQ(App::findHandlesStatus(done, true),
              "12 processes searched \xC2\xB7 4 found (the first ones) \xC2\xB7 3 couldn't be read \xC2\xB7 2 failed "
              "\xC2\xB7 1 ended first \xC2\xB7 2 only partly read");

    // Nothing unusual: just the counts, with no "(the first ones)" and none of the optional parts.
    Domain::HandleSearch::SearchPublication clean = Domain::HandleSearch::startFor("libssl", 5, 1);
    clean.searched = 5;
    clean.done = true;
    EXPECT_EQ(App::findHandlesStatus(clean, true), "5 processes searched \xC2\xB7 0 found");
}

// #3343: Stop signals the search's own stop source, so a search stopped while still queued on the worker (before it
// ran) reads no process, asks for no password, and publishes itself done.
TEST(FindHandlesSearchTest, ASearchStoppedBeforeItRunsReadsNothingAndEndsDone)
{
    TestMocks::MockProcessOpenFilesReader files;
    const std::vector<Domain::HandleSearch::SearchProcess> processes{
        {.target = {.pid = 1, .startTimeTicks = 1}, .name = "a"},
        {.target = {.pid = 2, .startTimeTicks = 2}, .name = "b"},
    };
    std::vector<Domain::HandleSearch::SearchPublication> published;
    const Domain::HandleSearch::Publish publish = [&published](Domain::HandleSearch::SearchPublication generation)
    {
        published.push_back(std::move(generation));
    };

    std::stop_source cancel;
    cancel.request_stop(); // Stop, pressed while the job was queued
    const std::stop_source worker;
    App::runFindHandlesSearch(processes,
                              {.openFiles = &files, .modules = nullptr},
                              true,
                              Domain::HandleSearch::startFor("x", processes.size(), 1),
                              cancel,
                              worker.get_token(),
                              publish);
    EXPECT_EQ(files.readCount(), 0);
    EXPECT_EQ(files.authorizedReadCount(), 0) << "no password prompt for a stopped search";
    ASSERT_FALSE(published.empty());
    EXPECT_TRUE(published.back().done);
    EXPECT_EQ(published.back().searched, 0U);
}

// The worker's own stop (Stop once the job is in flight, stop(), its destructor) stops the search too, and a search
// nobody stops reads every process.
TEST(FindHandlesSearchTest, TheWorkersStopReachesTheSearchAndAnUnstoppedOneReadsEveryProcess)
{
    TestMocks::MockProcessOpenFilesReader files;
    const std::vector<Domain::HandleSearch::SearchProcess> processes{
        {.target = {.pid = 1, .startTimeTicks = 1}, .name = "a"},
        {.target = {.pid = 2, .startTimeTicks = 2}, .name = "b"},
    };
    std::vector<Domain::HandleSearch::SearchPublication> published;
    const Domain::HandleSearch::Publish publish = [&published](Domain::HandleSearch::SearchPublication generation)
    {
        published.push_back(std::move(generation));
    };

    const std::stop_source cancel;
    std::stop_source worker;
    worker.request_stop();
    App::runFindHandlesSearch(processes,
                              {.openFiles = &files, .modules = nullptr},
                              false,
                              Domain::HandleSearch::startFor("x", processes.size(), 1),
                              cancel,
                              worker.get_token(),
                              publish);
    EXPECT_EQ(files.readCount(), 0);

    const std::stop_source fresh;
    const std::stop_source running;
    published.clear();
    App::runFindHandlesSearch(processes,
                              {.openFiles = &files, .modules = nullptr},
                              false,
                              Domain::HandleSearch::startFor("x", processes.size(), 2),
                              fresh,
                              running.get_token(),
                              publish);
    EXPECT_EQ(files.readCount(), 2);
    ASSERT_FALSE(published.empty());
    EXPECT_TRUE(published.back().done);
    EXPECT_EQ(published.back().searched, 2U);
}

} // namespace
