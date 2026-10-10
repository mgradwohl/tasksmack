/// @file test_CrashHistory.cpp
/// @brief Domain::CrashHistory (#1675): the shared crash cache's reads and publications, and the pure
/// matching of one executable against the list: case on Windows, a path or a name, the Linux comm's
/// 15-byte cut and the journal's executable path, the counts, the newest few and the newest time.
/// A fake read function stands in for the probe: no event log or core directory is read.

#include "Domain/CrashHistory.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Domain
{
namespace
{

using Platform::CrashesInfo;
using Platform::CrashEvent;
using Platform::OsFamily;

[[nodiscard]] CrashEvent crash(std::string application, std::uint64_t unixSeconds, bool hang = false)
{
    CrashEvent event;
    event.application = std::move(application);
    event.unixSeconds = unixSeconds;
    event.hang = hang;
    return event;
}

[[nodiscard]] CrashesInfo listed(OsFamily family, std::vector<CrashEvent> events)
{
    CrashesInfo info;
    info.available = true;
    info.listed = true;
    info.family = family;
    info.events = std::move(events);
    return info;
}

TEST(CrashMatchTest, WindowsMatchesTheFileNameIgnoringCase)
{
    const CrashEvent event = crash("Notepad.EXE", 1);
    EXPECT_TRUE(crashMatchesExecutable(event, "notepad.exe", OsFamily::Windows));
    EXPECT_TRUE(crashMatchesExecutable(event, R"(C:\Windows\System32\NOTEPAD.exe)", OsFamily::Windows)); // a path: its file name
    EXPECT_FALSE(crashMatchesExecutable(event, "notepad", OsFamily::Windows));                           // no partial match
    EXPECT_FALSE(crashMatchesExecutable(event, "notepad.exe.bak", OsFamily::Windows));
    EXPECT_FALSE(crashMatchesExecutable(event, "", OsFamily::Windows));
}

TEST(CrashMatchTest, LinuxMatchesTheCommsFifteenBytesAndIsCaseSensitive)
{
    // The kernel cuts "gnome-shell-calendar-server" to the comm "gnome-shell-cal".
    const CrashEvent cut = crash("gnome-shell-cal", 1);
    EXPECT_TRUE(crashMatchesExecutable(cut, "gnome-shell-calendar-server", OsFamily::Linux));
    EXPECT_TRUE(crashMatchesExecutable(cut, "/usr/libexec/gnome-shell-calendar-server", OsFamily::Linux));
    EXPECT_FALSE(crashMatchesExecutable(cut, "gnome-shell", OsFamily::Linux));

    const CrashEvent python = crash("python3", 1);
    EXPECT_TRUE(crashMatchesExecutable(python, "python3", OsFamily::Linux));
    EXPECT_FALSE(crashMatchesExecutable(python, "Python3", OsFamily::Linux));

    // The journal's executable path (#1674) matches the whole name, whatever the comm.
    CrashEvent journal = crash("python3", 1);
    journal.executable = "/usr/bin/python3.12";
    EXPECT_TRUE(crashMatchesExecutable(journal, "python3.12", OsFamily::Linux));
    EXPECT_TRUE(crashMatchesExecutable(journal, "python3", OsFamily::Linux));
}

TEST(CrashMatchTest, UnknownFamilyMatchesExactly)
{
    EXPECT_TRUE(crashMatchesExecutable(crash("app", 1), "app", OsFamily::Unknown));
    EXPECT_FALSE(crashMatchesExecutable(crash("App", 1), "app", OsFamily::Unknown));
}

TEST(CrashCountsTest, CountsCrashesAndHangsWithTheNewestFirst)
{
    const CrashesInfo info = listed(OsFamily::Windows,
                                    {crash("dllhost.exe", 500, true),
                                     crash("other.exe", 450),
                                     crash("DLLHOST.EXE", 400),
                                     crash("dllhost.exe", 300),
                                     crash("dllhost.exe", 200),
                                     crash("dllhost.exe", 150),
                                     crash("dllhost.exe", 100)});
    const ExecutableCrashCounts counts = crashCountsFor(info, "dllhost.exe");
    EXPECT_EQ(counts.crashes, 5U);
    EXPECT_EQ(counts.hangs, 1U);
    EXPECT_EQ(counts.total(), 6U);
    EXPECT_EQ(counts.lastUnixSeconds, 500U);
    ASSERT_EQ(counts.recent.size(), CRASH_RECENT_MAX);
    EXPECT_TRUE(counts.recent[0].hang);
    EXPECT_EQ(counts.recent[1].unixSeconds, 400U);
    EXPECT_EQ(counts.recent[4].unixSeconds, 150U);
    EXPECT_FALSE(counts.truncatedName);
}

TEST(CrashCountsTest, NoneAndUnlistedCountNothing)
{
    const ExecutableCrashCounts none = crashCountsFor(listed(OsFamily::Windows, {crash("other.exe", 9)}), "notepad.exe");
    EXPECT_EQ(none.total(), 0U);
    EXPECT_EQ(none.lastUnixSeconds, 0U);
    EXPECT_TRUE(none.recent.empty());

    CrashesInfo denied = listed(OsFamily::Windows, {crash("notepad.exe", 9)});
    denied.listed = false; // e.g. the log was refused: its events, if any, are not trusted
    denied.accessDenied = true;
    EXPECT_EQ(crashCountsFor(denied, "notepad.exe").total(), 0U);
}

TEST(CrashCountsTest, LinuxSaysWhenTheNameWasMatchedCut)
{
    const CrashesInfo info = listed(OsFamily::Linux, {crash("gnome-shell-cal", 7)});
    const ExecutableCrashCounts counts = crashCountsFor(info, "gnome-shell-calendar-server");
    EXPECT_EQ(counts.crashes, 1U);
    EXPECT_TRUE(counts.truncatedName);
    EXPECT_FALSE(crashCountsFor(info, "short").truncatedName);
    EXPECT_FALSE(crashCountsFor(listed(OsFamily::Windows, {}), "a-very-long-windows-name.exe").truncatedName);
}

TEST(CrashHistoryTest, ReadsOnlyWhenAskedAndPublishesEachRead)
{
    int reads = 0;
    CrashHistory history(
        [&reads]
        {
            ++reads;
            return listed(OsFamily::Windows, {crash("app.exe", static_cast<std::uint64_t>(reads))});
        });
    EXPECT_EQ(history.version(), 0U);
    EXPECT_EQ(reads, 0);
    EXPECT_FALSE(history.snapshot()->crashes.listed);

    history.read();
    const auto first = history.snapshot();
    EXPECT_EQ(first->version, 1U);
    EXPECT_GT(first->readAtUnixSeconds, 0U);
    ASSERT_EQ(first->crashes.events.size(), 1U);

    history.read();
    EXPECT_EQ(history.version(), 2U);
    EXPECT_EQ(history.snapshot()->crashes.events[0].unixSeconds, 2U);
    EXPECT_EQ(first->crashes.events[0].unixSeconds, 1U); // the earlier snapshot is unchanged
}

TEST(CrashHistoryTest, AThrowingReadIsPublishedAsUnlisted)
{
    CrashHistory history([]() -> CrashesInfo { throw std::runtime_error("log gone"); });
    history.read();
    const auto snapshot = history.snapshot();
    EXPECT_EQ(snapshot->version, 1U);
    EXPECT_TRUE(snapshot->crashes.available);
    EXPECT_FALSE(snapshot->crashes.listed);
    EXPECT_TRUE(snapshot->crashes.unavailableReason.contains("log gone"));
}

TEST(CrashHistoryTest, AReadThrowingANonStandardExceptionIsPublishedAsUnlisted)
{
    // read() runs on a worker: nothing it throws may reach the future (#1706).
    // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass) - intentionally not a std::exception
    CrashHistory history([] -> CrashesInfo { throw 42; });
    EXPECT_NO_THROW(history.read());
    const auto snapshot = history.snapshot();
    EXPECT_EQ(snapshot->version, 1U);
    EXPECT_TRUE(snapshot->crashes.available);
    EXPECT_FALSE(snapshot->crashes.listed);
    EXPECT_EQ(snapshot->crashes.unavailableReason, "Couldn't read the crash history: unknown error");
}

TEST(CrashHistoryTest, AnOlderPublicationThanTheShownOneIsDropped)
{
    CrashHistory history([] { return CrashesInfo{}; });
    history.publish(listed(OsFamily::Windows, {crash("new.exe", 1)}), 2000);
    history.publish(listed(OsFamily::Windows, {crash("old.exe", 1)}), 1000);
    EXPECT_EQ(history.version(), 1U);
    EXPECT_EQ(history.snapshot()->crashes.events[0].application, "new.exe");
    history.publish(listed(OsFamily::Windows, {}), 2000); // the same time is as fresh
    EXPECT_EQ(history.version(), 2U);
}

TEST(CrashHistoryTest, RequiresAReadFunction)
{
    EXPECT_THROW(CrashHistory(CrashHistory::ReadFn{}), std::invalid_argument);
}

} // namespace
} // namespace Domain
