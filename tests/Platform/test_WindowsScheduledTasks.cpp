/// @file test_WindowsScheduledTasks.cpp
/// @brief The Startup tab's scheduled tasks (#3424) against a fake Task Scheduler (ScheduledTaskApi): the walk
/// visits nested folders and keeps only the tasks with an enabled logon or boot trigger; the switch re-reads the
/// task, refuses Windows' own and gone ones, words a denied write as "Requires administrator", and never deletes;
/// WindowsStartupActions routes a task to it; WindowsStartupProbe lists the tasks, by name and scope, and walks
/// again only at its cadence or after a switch. The real Task Scheduler is never read or written here.

#include "Domain/SamplingConfig.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Windows/WindowsScheduledTasks.h"
#include "Platform/Windows/WindowsScheduledTasksMath.h"
#include "Platform/Windows/WindowsStartupActions.h"
#include "Platform/Windows/WindowsStartupProbe.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

namespace Math = Windows::ScheduledTaskMath;
using Math::TaskRecord;

constexpr std::int32_t FAKE_S_OK = 0;
constexpr auto ACCESS_DENIED = static_cast<std::int32_t>(Math::HRESULT_ACCESS_DENIED);
constexpr auto NOT_FOUND = static_cast<std::int32_t>(Math::HRESULT_FILE_NOT_FOUND);
constexpr auto RPC_UNAVAILABLE = static_cast<std::int32_t>(0x800706BAU); // RPC_S_SERVER_UNAVAILABLE

/// The fake Task Scheduler's folders and tasks, and what was asked of it.
struct FakeScheduler
{
    std::int32_t connectResult = FAKE_S_OK;
    std::int32_t writeResult = FAKE_S_OK;
    std::map<std::string, std::vector<std::string>> subfolders; ///< By folder path.
    std::map<std::string, std::vector<TaskRecord>> tasks;       ///< By folder path.
    std::vector<std::string> users{"S-1-5-21-1-2-3-1001", "PC\\alice", "alice"};
    int connects = 0;
    int apisMade = 0;
    std::vector<std::string> foldersRead;
    std::vector<std::pair<std::string, bool>> writes;

    [[nodiscard]] TaskRecord* find(const std::string& path)
    {
        for (auto& [folder, records] : tasks)
        {
            if (const auto it = std::ranges::find(records, path, &TaskRecord::path); it != records.end())
            {
                return &*it;
            }
        }
        return nullptr;
    }
};

class FakeApi final : public Windows::ScheduledTaskApi
{
  public:
    explicit FakeApi(FakeScheduler& state) : m_State(state)
    {}

    [[nodiscard]] std::int32_t connect() override
    {
        ++m_State.connects;
        return m_State.connectResult;
    }
    [[nodiscard]] std::vector<std::string> subfolders(const std::string& folder) override
    {
        const auto it = m_State.subfolders.find(folder);
        return it == m_State.subfolders.end() ? std::vector<std::string>{} : it->second;
    }
    [[nodiscard]] std::vector<TaskRecord> tasks(const std::string& folder) override
    {
        m_State.foldersRead.push_back(folder);
        const auto it = m_State.tasks.find(folder);
        return it == m_State.tasks.end() ? std::vector<TaskRecord>{} : it->second;
    }
    [[nodiscard]] std::expected<TaskRecord, std::int32_t> task(const std::string& path) override
    {
        const TaskRecord* found = m_State.find(path);
        if (found == nullptr)
        {
            return std::unexpected(NOT_FOUND);
        }
        return *found;
    }
    [[nodiscard]] std::int32_t setEnabled(const std::string& path, bool enabled) override
    {
        m_State.writes.emplace_back(path, enabled);
        if (m_State.writeResult != FAKE_S_OK)
        {
            return m_State.writeResult;
        }
        if (TaskRecord* found = m_State.find(path); found != nullptr)
        {
            found->enabled = enabled;
        }
        return FAKE_S_OK;
    }
    [[nodiscard]] std::vector<std::string> currentUserNames() override
    {
        return m_State.users;
    }

  private:
    FakeScheduler& m_State;
};

[[nodiscard]] Windows::ScheduledTaskApiFactory factoryFor(FakeScheduler& state)
{
    return [&state]
    {
        ++state.apisMade;
        return std::make_unique<FakeApi>(state);
    };
}

[[nodiscard]] TaskRecord makeTask(std::string path, std::vector<Math::TaskTrigger> triggers, std::string userId = "alice")
{
    TaskRecord task;
    task.path = std::move(path);
    task.triggers = std::move(triggers);
    task.userId = std::move(userId);
    return task;
}

const Math::TaskTrigger LOGON{.type = Math::TRIGGER_LOGON, .enabled = true};
const Math::TaskTrigger BOOT{.type = Math::TRIGGER_BOOT, .enabled = true};
const Math::TaskTrigger DAILY{.type = 2, .enabled = true}; // TASK_TRIGGER_DAILY
const Math::TaskTrigger LOGON_OFF{.type = Math::TRIGGER_LOGON, .enabled = false};

[[nodiscard]] std::vector<std::string> pathsOf(const std::vector<TaskRecord>& tasks)
{
    std::vector<std::string> paths;
    paths.reserve(tasks.size());
    for (const TaskRecord& task : tasks)
    {
        paths.push_back(task.path);
    }
    std::ranges::sort(paths);
    return paths;
}

[[nodiscard]] StartupEntry taskEntryAt(std::string path, StartupLocation location = StartupLocation::ScheduledTaskUser)
{
    StartupEntry entry;
    entry.name = path.substr(path.rfind('\\') + 1);
    entry.registrationName = std::move(path);
    entry.location = location;
    return entry;
}

class WindowsScheduledTasksTest : public ::testing::Test
{
  protected:
    FakeScheduler m_Scheduler;
};

// --- The walk -------------------------------------------------------------------------------------------------

TEST_F(WindowsScheduledTasksTest, WalksNestedFoldersAndKeepsOnlyLogonAndBootTasks)
{
    m_Scheduler.tasks["\\"] = {makeTask("\\AtSignIn", {LOGON}), makeTask("\\Nightly", {DAILY})};
    m_Scheduler.subfolders["\\"] = {"\\Vendor", "\\Other"};
    m_Scheduler.tasks["\\Vendor"] = {makeTask("\\Vendor\\AtBoot", {DAILY, BOOT})};
    m_Scheduler.subfolders["\\Vendor"] = {"\\Vendor\\Deep"};
    m_Scheduler.tasks[R"(\Vendor\Deep)"] = {
        makeTask(R"(\Vendor\Deep\TriggerOff)", {LOGON_OFF}),
        makeTask(R"(\Vendor\Deep\NoTriggers)", {}),
        makeTask(R"(\Vendor\Deep\Both)", {DAILY, LOGON}),
    };

    FakeApi api(m_Scheduler);
    const auto found = Windows::readStartupTasks(api);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(pathsOf(found.value_or(std::vector<TaskRecord>{})),
              (std::vector<std::string>{"\\AtSignIn", "\\Vendor\\AtBoot", "\\Vendor\\Deep\\Both"}));
    std::vector<std::string> visited = m_Scheduler.foldersRead;
    std::ranges::sort(visited);
    EXPECT_EQ(visited, (std::vector<std::string>{"\\", "\\Other", "\\Vendor", "\\Vendor\\Deep"}));
    EXPECT_EQ(m_Scheduler.connects, 1);
}

TEST_F(WindowsScheduledTasksTest, TheWalkStopsAtTheDepthLimit)
{
    std::string folder = "\\";
    for (int depth = 0; depth <= Math::MAX_FOLDER_DEPTH + 3; ++depth)
    {
        const std::string child = (folder == "\\" ? std::string() : folder) + std::format("\\F{}", depth);
        m_Scheduler.tasks[folder] = {makeTask((folder == "\\" ? std::string() : folder) + "\\Task", {LOGON})};
        m_Scheduler.subfolders[folder] = {child};
        folder = child;
    }
    FakeApi api(m_Scheduler);
    const auto found = Windows::readStartupTasks(api);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found.value_or(std::vector<TaskRecord>{}).size(), static_cast<std::size_t>(Math::MAX_FOLDER_DEPTH) + 1);
}

TEST_F(WindowsScheduledTasksTest, NoConnectionReadsNothing)
{
    m_Scheduler.connectResult = RPC_UNAVAILABLE;
    m_Scheduler.tasks["\\"] = {makeTask("\\AtSignIn", {LOGON})};
    FakeApi api(m_Scheduler);
    EXPECT_FALSE(Windows::readStartupTasks(api).has_value());
    EXPECT_TRUE(m_Scheduler.foldersRead.empty());
}

// --- The switch ----------------------------------------------------------------------------------------------

TEST_F(WindowsScheduledTasksTest, DisablesAndEnablesByPathAndCountsTheChange)
{
    m_Scheduler.tasks["\\Vendor"] = {makeTask("\\Vendor\\Updater", {LOGON})};
    FakeApi api(m_Scheduler);
    const std::uint64_t before = Windows::scheduledTaskChanges().load();

    const StartupActionResult off = Windows::setStartupTaskEnabled(api, taskEntryAt("\\Vendor\\Updater"), false);
    EXPECT_TRUE(off.ok) << off.message;
    const StartupActionResult on = Windows::setStartupTaskEnabled(api, taskEntryAt("\\Vendor\\Updater"), true);
    EXPECT_TRUE(on.ok) << on.message;

    EXPECT_EQ(m_Scheduler.writes, (std::vector<std::pair<std::string, bool>>{{"\\Vendor\\Updater", false}, {"\\Vendor\\Updater", true}}));
    EXPECT_EQ(Windows::scheduledTaskChanges().load(), before + 2);
    EXPECT_EQ(m_Scheduler.tasks["\\Vendor"].size(), 1U) << "a switch never removes the task";
}

TEST_F(WindowsScheduledTasksTest, ATaskAlreadySoIsNotWrittenButTheListIsReadAgain)
{
    TaskRecord task = makeTask("\\Off", {LOGON});
    task.enabled = false;
    m_Scheduler.tasks["\\"] = {task};
    FakeApi api(m_Scheduler);
    const std::uint64_t before = Windows::scheduledTaskChanges().load();
    EXPECT_TRUE(Windows::setStartupTaskEnabled(api, taskEntryAt("\\Off"), false).ok);
    EXPECT_TRUE(m_Scheduler.writes.empty());
    EXPECT_EQ(Windows::scheduledTaskChanges().load(), before + 1);
}

TEST_F(WindowsScheduledTasksTest, WindowsOwnTasksAreRefusedWithoutAsking)
{
    m_Scheduler.tasks[R"(\Microsoft\Windows\Setup)"] = {makeTask(R"(\Microsoft\Windows\Setup\Cleanup)", {BOOT}, "S-1-5-18")};
    FakeApi api(m_Scheduler);
    const StartupActionResult refused = Windows::setStartupTaskEnabled(
        api, taskEntryAt(R"(\microsoft\Windows\Setup\Cleanup)", StartupLocation::ScheduledTaskMachine), false);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.message, Math::MICROSOFT_TASK_REASON);
    EXPECT_EQ(m_Scheduler.connects, 0);
    EXPECT_TRUE(m_Scheduler.writes.empty());
}

TEST_F(WindowsScheduledTasksTest, RefusalsSayWhy)
{
    m_Scheduler.tasks["\\"] = {makeTask("\\System", {BOOT}, "S-1-5-18")};
    FakeApi api(m_Scheduler);

    const StartupActionResult gone = Windows::setStartupTaskEnabled(api, taskEntryAt("\\Removed"), false);
    EXPECT_FALSE(gone.ok);
    EXPECT_EQ(gone.message, "The scheduled task is no longer registered");

    m_Scheduler.writeResult = ACCESS_DENIED;
    const StartupActionResult denied =
        Windows::setStartupTaskEnabled(api, taskEntryAt("\\System", StartupLocation::ScheduledTaskMachine), false);
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.message, "Requires administrator");

    m_Scheduler.connectResult = RPC_UNAVAILABLE;
    const StartupActionResult unreachable = Windows::setStartupTaskEnabled(api, taskEntryAt("\\System"), false);
    EXPECT_FALSE(unreachable.ok);
    EXPECT_EQ(unreachable.message, "Task Scheduler error 0x800706BA");

    const StartupActionResult noPath = Windows::setStartupTaskEnabled(api, taskEntryAt(""), false);
    EXPECT_FALSE(noPath.ok);
    StartupEntry runEntry = taskEntryAt("\\System");
    runEntry.location = StartupLocation::RunUser;
    EXPECT_FALSE(Windows::setStartupTaskEnabled(api, runEntry, false).ok);
    EXPECT_EQ(m_Scheduler.writes.size(), 1U) << "only the denied write was tried";
}

TEST_F(WindowsScheduledTasksTest, StartupActionsRouteATaskToTheTaskScheduler)
{
    m_Scheduler.tasks["\\"] = {makeTask("\\Mine", {LOGON})};
    WindowsStartupActions actions(false, {}, factoryFor(m_Scheduler));
    const StartupActionResult result = actions.setEnabled(taskEntryAt("\\Mine"), false);
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(m_Scheduler.apisMade, 1);
    EXPECT_EQ(m_Scheduler.writes, (std::vector<std::pair<std::string, bool>>{{"\\Mine", false}}));

    WindowsStartupActions without(false, {}, nullptr);
    EXPECT_FALSE(without.setEnabled(taskEntryAt("\\Mine"), true).ok);
}

// --- The probe -----------------------------------------------------------------------------------------------

LSTATUS WINAPI noKey(HKEY /*root*/, LPCWSTR /*subkey*/, DWORD /*options*/, REGSAM /*access*/, PHKEY /*result*/)
{
    return ERROR_FILE_NOT_FOUND;
}

HRESULT WINAPI noFolder(const GUID& /*id*/, DWORD /*flags*/, HANDLE /*token*/, PWSTR* path)
{
    *path = nullptr;
    return E_FAIL;
}

HRESULT WINAPI authorText(PCWSTR source, PWSTR text, UINT textChars, void** /*reserved*/)
{
    if (std::wcscmp(source, L"@%SystemRoot%\\system32\\example.dll,-600") != 0)
    {
        return E_FAIL;
    }
    return wcscpy_s(text, textChars, L"Microsoft Corporation") == 0 ? S_OK : E_FAIL;
}

/// No Run keys, Startup folders or packaged apps: only the fake scheduler's tasks are listed.
[[nodiscard]] Windows::StartupProbeFunctions emptyRegistry()
{
    Windows::StartupProbeFunctions functions;
    functions.openKey = &noKey;
    functions.knownFolderPath = &noFolder;
    functions.loadIndirectString = &authorText;
    return functions;
}

[[nodiscard]] const StartupEntry* entryAt(const std::vector<StartupEntry>& entries, const std::string& path)
{
    const auto it = std::ranges::find(entries, path, &StartupEntry::registrationName);
    return it == entries.end() ? nullptr : &*it;
}

TEST_F(WindowsScheduledTasksTest, TheProbeListsTasksByNameScopeAndOwner)
{
    TaskRecord mine = makeTask("\\Vendor\\Updater", {LOGON}, "PC\\Alice");
    mine.execPath = "C:\\No Such Folder\\updater.exe";
    mine.execArguments = "/silent";
    mine.author = "Vendor Inc.";
    TaskRecord system = makeTask("\\Agent", {BOOT}, "SYSTEM");
    system.enabled = false;
    TaskRecord windows = makeTask(R"(\Microsoft\Windows\Example\Task)", {LOGON}, "");
    windows.author = "$(@%SystemRoot%\\system32\\example.dll,-600)";
    m_Scheduler.tasks["\\"] = {system};
    m_Scheduler.tasks["\\Vendor"] = {mine};
    m_Scheduler.tasks[R"(\Microsoft\Windows\Example)"] = {windows};
    m_Scheduler.subfolders["\\"] = {"\\Vendor", "\\Microsoft"};
    m_Scheduler.subfolders["\\Microsoft"] = {"\\Microsoft\\Windows"};
    m_Scheduler.subfolders[R"(\Microsoft\Windows)"] = {R"(\Microsoft\Windows\Example)"};

    WindowsStartupProbe probe(emptyRegistry(), factoryFor(m_Scheduler));
    const std::vector<StartupEntry> entries = probe.enumerate();
    ASSERT_EQ(entries.size(), 3U);

    const StartupEntry* updater = entryAt(entries, "\\Vendor\\Updater");
    ASSERT_NE(updater, nullptr);
    EXPECT_EQ(updater->name, "Updater");
    EXPECT_EQ(updater->location, StartupLocation::ScheduledTaskUser);
    EXPECT_EQ(updater->scope, StartupScope::User) << "PC\\Alice is the current user, whatever the case";
    EXPECT_EQ(updater->command, "\"C:\\No Such Folder\\updater.exe\" /silent");
    EXPECT_EQ(updater->executablePath, "C:\\No Such Folder\\updater.exe");
    EXPECT_EQ(updater->target, StartupTargetState::Missing);
    EXPECT_EQ(updater->publisher, "Vendor Inc.") << "the Author stands in for a publisher the program doesn't name";
    EXPECT_TRUE(updater->enabled);
    EXPECT_FALSE(updater->osComponent);
    EXPECT_TRUE(hasSwitchableState(*updater));

    const StartupEntry* agent = entryAt(entries, "\\Agent");
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->location, StartupLocation::ScheduledTaskMachine);
    EXPECT_EQ(agent->scope, StartupScope::Machine);
    EXPECT_FALSE(agent->enabled);
    EXPECT_TRUE(agent->command.empty());
    EXPECT_EQ(agent->target, StartupTargetState::Unresolved);

    const StartupEntry* own = entryAt(entries, R"(\Microsoft\Windows\Example\Task)");
    ASSERT_NE(own, nullptr);
    EXPECT_TRUE(own->osComponent);
    EXPECT_EQ(own->scope, StartupScope::Machine) << "a group's task isn't the current user's own";
    EXPECT_EQ(own->notSwitchableReason, Math::MICROSOFT_TASK_REASON);
    EXPECT_FALSE(hasSwitchableState(*own));
    EXPECT_EQ(own->publisher, "Microsoft Corporation") << "the indirect Author is loaded";
}

TEST_F(WindowsScheduledTasksTest, TheProbeWalksAtItsCadenceOrAfterASwitch)
{
    m_Scheduler.tasks["\\"] = {makeTask("\\First", {LOGON})};
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    WindowsStartupProbe probe(emptyRegistry(), factoryFor(m_Scheduler), [&now] { return now; });

    EXPECT_EQ(probe.enumerate().size(), 1U);
    EXPECT_EQ(m_Scheduler.connects, 1);

    // A task added since isn't seen until the cadence is up: the cached task is still listed meanwhile.
    m_Scheduler.tasks["\\"].push_back(makeTask("\\Second", {BOOT}));
    now += std::chrono::seconds(Domain::Sampling::STARTUP_SCHEDULED_TASKS_REFRESH_SECONDS - 1);
    EXPECT_EQ(probe.enumerate().size(), 1U);
    EXPECT_EQ(m_Scheduler.connects, 1);

    now += std::chrono::seconds(1);
    EXPECT_EQ(probe.enumerate().size(), 2U);
    EXPECT_EQ(m_Scheduler.connects, 2);

    // A switch through TaskSmack is read at the next sample.
    FakeApi api(m_Scheduler);
    ASSERT_TRUE(Windows::setStartupTaskEnabled(api, taskEntryAt("\\First"), false).ok);
    const int connectsBefore = m_Scheduler.connects;
    const std::vector<StartupEntry> after = probe.enumerate();
    EXPECT_EQ(m_Scheduler.connects, connectsBefore + 1);
    const StartupEntry* first = entryAt(after, "\\First");
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(first->enabled);

    // A Task Scheduler that can't be reached lists none, and is tried again at the cadence.
    m_Scheduler.connectResult = RPC_UNAVAILABLE;
    now += std::chrono::seconds(Domain::Sampling::STARTUP_SCHEDULED_TASKS_REFRESH_SECONDS);
    EXPECT_TRUE(probe.enumerate().empty());
}

TEST_F(WindowsScheduledTasksTest, AProbeWithoutTheSeamListsNoTasks)
{
    m_Scheduler.tasks["\\"] = {makeTask("\\First", {LOGON})};
    WindowsStartupProbe probe(emptyRegistry());
    EXPECT_TRUE(probe.enumerate().empty());
    EXPECT_EQ(m_Scheduler.connects, 0);
}

} // namespace
} // namespace Platform
