/// @file test_CrashHistory.cpp
/// @brief Domain::CrashHistory (#1675): the shared crash cache's reads and publications, and the pure
/// matching of one executable against the list: case on Windows, a path or a name, the Linux comm's
/// 15-byte cut and the journal's executable path, the counts, the newest few and the newest time.
/// A fake read function stands in for the probe: no event log or core directory is read.

#include "Domain/CrashHistory.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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

TEST(CrashMatchTest, WindowsFoldsAsciiLettersOnlyNeverAByteOfUtf8)
{
    // Mixed case in every letter folds; a byte of 0x80 and up is compared as itself whatever the
    // locale (#2566): 0xC4 is Latin-1's Ä, which a Latin-1 C locale would fold to 0xE4 (ä).
    EXPECT_TRUE(crashMatchesExecutable(crash("mY\xC4pP.ExE", 1), "My\xC4Pp.eXe", OsFamily::Windows));
    EXPECT_FALSE(crashMatchesExecutable(crash("\xC4pp.exe", 1), "\xE4pp.exe", OsFamily::Windows));
    EXPECT_TRUE(crashMatchesExecutable(crash("Caf\xC3\xA9.exe", 1), "CAF\xC3\xA9.EXE", OsFamily::Windows));  // é in UTF-8
    EXPECT_FALSE(crashMatchesExecutable(crash("Caf\xC3\xA9.exe", 1), "Caf\xC3\x89.exe", OsFamily::Windows)); // É: not é
    EXPECT_FALSE(crashMatchesExecutable(crash("", 1), "app.exe", OsFamily::Windows));                        // no application name
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
    const std::uint64_t older = history.beginRead();
    const std::uint64_t newer = history.beginRead();
    history.publish(listed(OsFamily::Windows, {crash("new.exe", 1)}), 2000, newer);
    history.publish(listed(OsFamily::Windows, {crash("old.exe", 1)}), 2000, older);
    EXPECT_EQ(history.version(), 1U);
    EXPECT_EQ(history.snapshot()->crashes.events[0].application, "new.exe");
    history.publish(listed(OsFamily::Windows, {}), 2000); // a read starting now is newer
    EXPECT_EQ(history.version(), 2U);
}

// #2157: publications are ordered by when the read started, not by the wall clock. A read that started
// later but is stamped an hour earlier (the clock stepped back, say an NTP correction after a resume)
// replaces the shown one; it used to be dropped until the clock caught up.
TEST(CrashHistoryTest, AReadAfterTheClockSteppedBackReplacesTheShownOne)
{
    constexpr std::uint64_t shownAt = 1'790'003'600;
    constexpr std::uint64_t steppedBack = shownAt - 3600;

    CrashHistory history([] { return CrashesInfo{}; });
    const std::uint64_t first = history.beginRead();
    const std::uint64_t second = history.beginRead();
    history.publish(listed(OsFamily::Windows, {crash("first.exe", 1)}), shownAt, first);
    history.publish(listed(OsFamily::Windows, {crash("second.exe", 1)}), steppedBack, second);
    EXPECT_EQ(history.version(), 2U);
    ASSERT_EQ(history.snapshot()->crashes.events.size(), 1U);
    EXPECT_EQ(history.snapshot()->crashes.events[0].application, "second.exe");
    EXPECT_EQ(history.snapshot()->readAtUnixSeconds, steppedBack); // shown as stamped

    // The same through the overload that takes its ticket as it publishes (Process Details' tests).
    history.publish(listed(OsFamily::Windows, {crash("third.exe", 1)}), steppedBack - 3600);
    EXPECT_EQ(history.version(), 3U);
    EXPECT_EQ(history.snapshot()->crashes.events[0].application, "third.exe");

    // And a read() of its own, stamped by the real clock, after a publication stamped in the future.
    CrashHistory reading([] { return listed(OsFamily::Windows, {crash("read.exe", 1)}); });
    reading.publish(listed(OsFamily::Windows, {crash("future.exe", 1)}), 4'000'000'000);
    reading.read();
    EXPECT_EQ(reading.version(), 2U);
    EXPECT_EQ(reading.snapshot()->crashes.events[0].application, "read.exe");
}

/// Wall-clock seconds now, as CrashHistory stamps reads.
[[nodiscard]] std::uint64_t secondsNow()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// #1773: a read is stamped when it starts. A slow one that started before a newer publication doesn't
// replace it when it finishes; before the fix it was stamped when it finished, so it did.
TEST(CrashHistoryTest, ASlowReadThatStartedEarlierDoesNotReplaceANewerList)
{
    std::atomic<bool> reading{false};
    CrashHistory history(
        [&reading]
        {
            reading = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            return listed(OsFamily::Windows, {crash("old.exe", 1)});
        });
    std::thread slow([&history] { history.read(); });
    while (!reading)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // While it reads, a newer list arrives (the System Information read). Its ticket is taken now the
    // read has started -- read() takes its own before calling the reader -- so it is the newer one,
    // whatever the wall-clock stamps say (#2157).
    history.publish(listed(OsFamily::Windows, {crash("new.exe", 1)}), secondsNow());
    slow.join();
    ASSERT_FALSE(history.snapshot()->crashes.events.empty());
    EXPECT_EQ(history.snapshot()->crashes.events[0].application, "new.exe");
    EXPECT_EQ(history.version(), 1U); // the slow read was dropped

    // On its own, a slow read carries the second it started, not the one it finished.
    CrashHistory alone(
        []
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            return listed(OsFamily::Linux, {});
        });
    const std::uint64_t before = secondsNow();
    alone.read();
    EXPECT_LE(alone.snapshot()->readAtUnixSeconds, before + 1);
}

TEST(CrashHistoryTest, RequiresAReadFunction)
{
    EXPECT_THROW(CrashHistory(CrashHistory::ReadFn{}), std::invalid_argument);
}

} // namespace
} // namespace Domain
