/// @file test_FindHandlesFormat.cpp
/// @brief The Find handles dialog's pure parts (#3067, #3374): the results' order for every column and direction,
/// with its tie-breaks, and every part of the status line.

#include "App/Panels/FindHandlesDialog.h"
#include "Domain/HandleSearch.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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

    // Nothing unusual: just the counts, with no "(the first ones)" and none of the optional parts.
    Domain::HandleSearch::SearchPublication clean = Domain::HandleSearch::startFor("libssl", 5, 1);
    clean.searched = 5;
    clean.done = true;
    EXPECT_EQ(App::findHandlesStatus(clean, true), "5 processes searched \xC2\xB7 0 found");
}

} // namespace
