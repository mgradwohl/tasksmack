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

} // namespace
} // namespace App
