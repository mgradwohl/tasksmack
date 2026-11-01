/// @file test_StartAtLoginSetting.cpp
/// @brief Settings' start at sign-in, run (#3093, part 2): the entry is read and changed on the worker,
/// never the caller's thread; On writes this copy's quoted path and --minimized, Off deletes it, Repair
/// rewrites another copy's, and a failing read or write is a Failed status saying why. A fake entry: never
/// the registry.

#include "App/StartAtLoginSetting.h"
#include "Domain/StartAtLogin.h"
#include "Mocks/FakeStartAtLogin.h"

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Domain::StartAtLogin::State;
using TestSupport::FakeStartAtLogin;

constexpr const char* THIS_COPY = R"(C:\Program Files\TaskSmack\TaskSmack.exe)";
constexpr const char* THIS_COPY_COMMAND = R"("C:\Program Files\TaskSmack\TaskSmack.exe" --minimized)";

class StartAtLoginSettingTest : public ::testing::Test
{
  protected:
    std::shared_ptr<FakeStartAtLogin> m_Entry = std::make_shared<FakeStartAtLogin>();
    StartAtLoginSetting m_Setting{m_Entry};

    [[nodiscard]] std::optional<std::string> command()
    {
        return m_Entry->with([](const FakeStartAtLogin::Data& data) { return data.command; });
    }

    void setCommand(std::optional<std::string> value)
    {
        m_Entry->with([&value](FakeStartAtLogin::Data& data) { data.command = std::move(value); });
    }
};

TEST(StartAtLoginSettingNoEntryTest, WithoutAnEntryNothingIsOfferedOrDone)
{
    StartAtLoginSetting setting;
    EXPECT_FALSE(setting.offered());
    setting.refresh();
    setting.setEnabled(true);
    EXPECT_FALSE(setting.busy());
    EXPECT_FALSE(setting.poll());
    EXPECT_EQ(setting.status().state, State::Unknown);
}

TEST_F(StartAtLoginSettingTest, NothingIsReadUntilAsked)
{
    EXPECT_TRUE(m_Setting.offered());
    EXPECT_EQ(m_Setting.status().state, State::Unknown);
    EXPECT_FALSE(m_Setting.poll());
    EXPECT_EQ(m_Entry->with([](const FakeStartAtLogin::Data& data) { return data.reads; }), 0);
}

TEST_F(StartAtLoginSettingTest, NoEntryReadsAsOff)
{
    m_Setting.refresh();
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::Off);
    EXPECT_FALSE(m_Setting.busy());
}

TEST_F(StartAtLoginSettingTest, TurningItOnWritesThisCopyMinimized)
{
    m_Setting.setEnabled(true);
    m_Setting.waitForTesting();
    EXPECT_EQ(command(), std::optional<std::string>(THIS_COPY_COMMAND));
    EXPECT_EQ(m_Setting.status().state, State::On) << "read back after the write";
    EXPECT_TRUE(m_Setting.status().installed);
}

TEST_F(StartAtLoginSettingTest, AnExistingEntryForThisCopyReadsAsOn)
{
    setCommand(Domain::StartAtLogin::runCommand(THIS_COPY));
    m_Setting.refresh();
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::On);
}

TEST_F(StartAtLoginSettingTest, TurningItOffDeletesTheEntry)
{
    setCommand(THIS_COPY_COMMAND);
    m_Setting.setEnabled(false);
    m_Setting.waitForTesting();
    EXPECT_EQ(command(), std::nullopt);
    EXPECT_EQ(m_Setting.status().state, State::Off);
    EXPECT_EQ(m_Entry->with([](const FakeStartAtLogin::Data& data) { return data.writes; }), 0);
}

TEST_F(StartAtLoginSettingTest, AnotherCopysEntryIsRepairedToThisOne)
{
    setCommand(R"("E:\Moved\TaskSmack.exe" --minimized)");
    m_Setting.refresh();
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::OtherCopy);
    EXPECT_EQ(m_Setting.status().otherProgram, R"(E:\Moved\TaskSmack.exe)");

    m_Setting.setEnabled(true); // Repair
    m_Setting.waitForTesting();
    EXPECT_EQ(command(), std::optional<std::string>(THIS_COPY_COMMAND));
    EXPECT_EQ(m_Setting.status().state, State::On);
}

TEST_F(StartAtLoginSettingTest, AFailedReadSaysWhy)
{
    m_Entry->with([](FakeStartAtLogin::Data& data) { data.readError = "couldn't open the Run key: Access is denied."; });
    m_Setting.refresh();
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_EQ(m_Setting.status().error, "couldn't open the Run key: Access is denied.");

    // Turning it on can't name this copy without the read either: nothing is written.
    m_Setting.setEnabled(true);
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_EQ(m_Entry->with([](const FakeStartAtLogin::Data& data) { return data.writes; }), 0);
}

TEST_F(StartAtLoginSettingTest, AFailedWriteOrDeleteSaysWhy)
{
    m_Entry->with([](FakeStartAtLogin::Data& data) { data.writeError = "couldn't write the Run value: Access is denied."; });
    m_Setting.setEnabled(true);
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_EQ(m_Setting.status().error, "couldn't write the Run value: Access is denied.");
    EXPECT_EQ(command(), std::nullopt);

    m_Entry->with(
        [](FakeStartAtLogin::Data& data)
        {
            data.command = THIS_COPY_COMMAND;
            data.removeError = "couldn't delete the Run value: Access is denied.";
        });
    m_Setting.setEnabled(false);
    m_Setting.waitForTesting();
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_EQ(m_Setting.status().error, "couldn't delete the Run value: Access is denied.");
    EXPECT_EQ(command(), std::optional<std::string>(THIS_COPY_COMMAND));
}

// The entry is read and written on the worker: the caller is never held up by a slow registry, and a
// change asked for while a read runs is made after it, the last one asked for winning.
TEST_F(StartAtLoginSettingTest, WorkRunsOnTheWorkerAndAChangeWaitsItsTurn)
{
    std::promise<void> release;
    m_Entry->with([future = release.get_future().share()](FakeStartAtLogin::Data& data) { data.hold = future; });

    m_Setting.refresh();
    EXPECT_TRUE(m_Setting.busy());
    EXPECT_FALSE(m_Setting.poll()) << "still reading";
    m_Setting.setEnabled(false);
    m_Setting.setEnabled(true);
    m_Setting.refresh(); // ignored while busy
    EXPECT_TRUE(m_Setting.busy());

    release.set_value();
    m_Setting.waitForTesting();
    EXPECT_FALSE(m_Setting.busy());
    EXPECT_EQ(command(), std::optional<std::string>(THIS_COPY_COMMAND));
    EXPECT_EQ(m_Setting.status().state, State::On);
    EXPECT_EQ(m_Entry->with([](const FakeStartAtLogin::Data& data) { return data.removes; }), 0) << "only the last change ran";
}

TEST_F(StartAtLoginSettingTest, PollSaysWhenTheStatusChanged)
{
    const auto pollUntilIdle = [this]
    {
        bool changed = false;
        while (m_Setting.busy())
        {
            changed = m_Setting.poll() || changed;
        }
        return changed;
    };
    m_Setting.refresh();
    EXPECT_TRUE(pollUntilIdle()) << "Unknown to Off";
    EXPECT_EQ(m_Setting.status().state, State::Off);
    m_Setting.refresh();
    EXPECT_FALSE(pollUntilIdle()) << "the same status again";
    setCommand(THIS_COPY_COMMAND);
    m_Setting.refresh();
    EXPECT_TRUE(pollUntilIdle()) << "Off to On";
}

// #3276: the startup check, through a fake that restores as Windows' Run value does (never the registry).
class StartAtLoginStartupCheckTest : public ::testing::Test
{
  protected:
    static constexpr const char* OLD_BUILD = "0.10.0+abc1234";
    static constexpr const char* THIS_BUILD = "0.10.1+def5678";

    std::shared_ptr<FakeStartAtLogin> m_Entry = std::make_shared<FakeStartAtLogin>(true);
    StartAtLoginSetting m_Setting{m_Entry};

    [[nodiscard]] FakeStartAtLogin::Data data()
    {
        return m_Entry->with([](const FakeStartAtLogin::Data& d) { return d; });
    }

    void check(bool intended, const char* seenBy)
    {
        m_Setting.startupCheck(intended, seenBy, THIS_BUILD);
        m_Setting.waitForTesting();
    }
};

TEST_F(StartAtLoginStartupCheckTest, AnEntryAnInstallRemovedIsWrittenBackForThisCopy)
{
    EXPECT_TRUE(m_Setting.restoresMissingEntry());
    check(true, OLD_BUILD);
    EXPECT_EQ(data().command, std::optional<std::string>(THIS_COPY_COMMAND));
    EXPECT_EQ(data().writes, 1);
    EXPECT_EQ(m_Setting.status().state, State::On) << "read back after the write";
}

// #3276: a real uninstall's mark. The entry went with TaskSmack: nothing is written back, the status says
// so, and the mark is cleared, so it counts once.
TEST_F(StartAtLoginStartupCheckTest, AfterAnUninstallNothingIsWrittenBack)
{
    m_Entry->with([](FakeStartAtLogin::Data& d) { d.uninstallMark = true; });
    check(true, OLD_BUILD);
    EXPECT_EQ(data().writes, 0);
    EXPECT_EQ(data().command, std::nullopt);
    EXPECT_EQ(m_Setting.status().state, State::Off);
    EXPECT_TRUE(m_Setting.status().uninstalled);
    EXPECT_FALSE(data().uninstallMark) << "taken";

    check(true, OLD_BUILD); // the next start: no mark, so an upgrade's missing entry is written back again
    EXPECT_FALSE(m_Setting.status().uninstalled);
    EXPECT_EQ(data().writes, 1);
}

TEST_F(StartAtLoginStartupCheckTest, AMarkThatCantBeReadWritesNothingBack)
{
    m_Entry->with([](FakeStartAtLogin::Data& d) { d.markError = "couldn't open the uninstall mark's key: Access is denied."; });
    check(true, OLD_BUILD);
    EXPECT_EQ(data().writes, 0) << "an uninstall can't be ruled out";
    EXPECT_EQ(m_Setting.status().state, State::Off);
    EXPECT_FALSE(m_Setting.status().uninstalled);
}

TEST_F(StartAtLoginStartupCheckTest, TheMarkIsReportedEvenWhenTheReadFails)
{
    m_Entry->with(
        [](FakeStartAtLogin::Data& d)
        {
            d.uninstallMark = true;
            d.readError = "couldn't open the Run key: Access is denied.";
        });
    check(false, "");
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_TRUE(m_Setting.status().uninstalled) << "taken, so it must be acted on now";
}

TEST_F(StartAtLoginStartupCheckTest, OnlyReadsWhenThereIsNothingToWriteBack)
{
    struct Case
    {
        const char* name;
        std::optional<std::string> command;
        bool intended;
        const char* seenBy;
        bool installed;
        State state;
    };
    const std::vector<Case> cases = {
        {.name = "never turned on", .command = std::nullopt, .intended = false, .seenBy = "", .installed = true, .state = State::Off},
        {.name = "removed while this build ran",
         .command = std::nullopt,
         .intended = true,
         .seenBy = THIS_BUILD,
         .installed = true,
         .state = State::Off},
        {.name = "a portable copy",
         .command = std::nullopt,
         .intended = true,
         .seenBy = OLD_BUILD,
         .installed = false,
         .state = State::Off},
        {.name = "there", .command = THIS_COPY_COMMAND, .intended = true, .seenBy = OLD_BUILD, .installed = true, .state = State::On},
        {.name = "another copy's",
         .command = R"("E:\Moved\TaskSmack.exe")",
         .intended = true,
         .seenBy = OLD_BUILD,
         .installed = true,
         .state = State::OtherCopy},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        m_Entry->with(
            [&c](FakeStartAtLogin::Data& d)
            {
                d.command = c.command;
                d.installed = c.installed;
                d.writes = 0;
            });
        check(c.intended, c.seenBy);
        EXPECT_EQ(data().writes, 0);
        EXPECT_EQ(data().command, c.command);
        EXPECT_EQ(m_Setting.status().state, c.state);
    }
}

TEST_F(StartAtLoginStartupCheckTest, APlatformThatDoesntRestoreNeverWrites)
{
    const auto linuxLike = std::make_shared<FakeStartAtLogin>(false);
    StartAtLoginSetting setting{linuxLike};
    EXPECT_FALSE(setting.restoresMissingEntry());
    setting.startupCheck(true, OLD_BUILD, THIS_BUILD);
    setting.waitForTesting();
    EXPECT_EQ(linuxLike->with([](const FakeStartAtLogin::Data& d) { return d.writes; }), 0);
    EXPECT_EQ(setting.status().state, State::Off);
}

TEST_F(StartAtLoginStartupCheckTest, AFailedReadOrWriteSaysWhy)
{
    m_Entry->with([](FakeStartAtLogin::Data& d) { d.writeError = "couldn't write the Run value: Access is denied."; });
    check(true, OLD_BUILD);
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_EQ(m_Setting.status().error, "couldn't write the Run value: Access is denied.");

    m_Entry->with(
        [](FakeStartAtLogin::Data& d)
        {
            d.writeError.clear();
            d.readError = "couldn't open the Run key: Access is denied.";
        });
    check(true, OLD_BUILD);
    EXPECT_EQ(m_Setting.status().state, State::Failed);
    EXPECT_EQ(m_Setting.status().error, "couldn't open the Run key: Access is denied.");
}

TEST_F(StartAtLoginStartupCheckTest, ItRunsOnTheWorkerAndIsIgnoredWhileBusy)
{
    std::promise<void> release;
    m_Entry->with([future = release.get_future().share()](FakeStartAtLogin::Data& d) { d.hold = future; });
    m_Setting.refresh();
    EXPECT_TRUE(m_Setting.busy());
    m_Setting.startupCheck(true, OLD_BUILD, THIS_BUILD); // ignored: a read runs
    release.set_value();
    m_Setting.waitForTesting();
    EXPECT_EQ(data().writes, 0);

    StartAtLoginSetting none;
    none.startupCheck(true, OLD_BUILD, THIS_BUILD);
    EXPECT_FALSE(none.busy());
    EXPECT_FALSE(none.restoresMissingEntry());
}

TEST(StartAtLoginBuildTest, TheBuildIsTheVersionAndTheCommit)
{
    const std::string build = buildIdentity();
    EXPECT_NE(build.find('+'), std::string::npos);
    EXPECT_EQ(build, buildIdentity()) << "the same for the whole run";
}

} // namespace
} // namespace App
