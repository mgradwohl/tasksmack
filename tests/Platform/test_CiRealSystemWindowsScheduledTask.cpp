// CI real-system test for the Startup tab's scheduled tasks (#3424; tracker #3492): a throwaway task with a logon
// trigger, registered for the current user in the root folder through the Task Scheduler API, is listed by the real
// WindowsStartupProbe, then disabled and enabled again by the real WindowsStartupActions (IRegisteredTask::put_Enabled),
// and deleted on scope exit. It changes the machine's task list, so it runs only on a disposable GitHub-hosted runner,
// where CiRealSystem.h's guard lets it; the logon trigger never fires there, since no one signs in to the runner.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/ComScope.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsScheduledTasks.h"
#include "Platform/Windows/WindowsStartupActions.h"
#include "Platform/Windows/WindowsStartupProbe.h"
#include "Support/CiRealSystem.h"
#include "Support/CiRealSystemWindows.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
// clang-format on

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")

#include <filesystem>
#include <ios>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef TASKSMACK_CI_STUB_PATH
#error "TASKSMACK_CI_STUB_PATH must name TaskSmackCiStub.exe (tests/CMakeLists.txt)"
#endif

namespace
{

using Platform::ComPtr;
namespace WinString = Platform::WinString;

/// Owns one BSTR made from a wide string.
class TestBstr
{
  public:
    explicit TestBstr(const std::wstring& text) : m_Value(SysAllocString(text.c_str()))
    {}
    ~TestBstr()
    {
        SysFreeString(m_Value);
    }
    TestBstr(const TestBstr&) = delete;
    TestBstr& operator=(const TestBstr&) = delete;
    TestBstr(TestBstr&&) = delete;
    TestBstr& operator=(TestBstr&&) = delete;

    [[nodiscard]] BSTR get() const noexcept
    {
        return m_Value;
    }

  private:
    BSTR m_Value;
};

/// Deletes the task the test registered, whatever the test's outcome; never any other task.
class TaskRemover
{
  public:
    TaskRemover(ComPtr<ITaskFolder> root, std::wstring name) : m_Root(std::move(root)), m_Name(std::move(name))
    {}
    ~TaskRemover()
    {
        if (m_Root)
        {
            const TestBstr name(m_Name);
            static_cast<void>(m_Root->DeleteTask(name.get(), 0));
        }
    }
    TaskRemover(const TaskRemover&) = delete;
    TaskRemover& operator=(const TaskRemover&) = delete;
    TaskRemover(TaskRemover&&) = delete;
    TaskRemover& operator=(TaskRemover&&) = delete;

  private:
    ComPtr<ITaskFolder> m_Root;
    std::wstring m_Name;
};

/// The entry the probe lists at @p path now; nullopt when it lists none.
[[nodiscard]] std::optional<Platform::StartupEntry> listedTask(Platform::WindowsStartupProbe& probe, const std::string& path)
{
    for (Platform::StartupEntry& entry : probe.enumerate())
    {
        if ((entry.location == Platform::StartupLocation::ScheduledTaskUser ||
             entry.location == Platform::StartupLocation::ScheduledTaskMachine) &&
            entry.registrationName == path)
        {
            return entry;
        }
    }
    return std::nullopt;
}

/// Whether the task at @p path is enabled, as Task Scheduler itself says; nullopt when it can't be read.
[[nodiscard]] std::optional<bool> taskEnabled(ITaskFolder* root, const std::wstring& path)
{
    ComPtr<IRegisteredTask> task;
    const TestBstr name(path);
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(root->GetTask(name.get(), task.releaseAndGetAddressOf())) || !task || FAILED(task->get_Enabled(&enabled)))
    {
        return std::nullopt;
    }
    return enabled != VARIANT_FALSE;
}

class CiRealSystemScheduledTaskTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemScheduledTaskTest, AUserLogonTaskIsListedDisabledAndEnabled)
{
    const Platform::ComScope com;
    ASSERT_TRUE(com.usable());
    ComPtr<ITaskService> service;
    ASSERT_TRUE(SUCCEEDED(CoCreateInstance(
        CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(service.releaseAndGetAddressOf()))));
    const VARIANT none{};
    ASSERT_TRUE(SUCCEEDED(service->Connect(none, none, none, none)));
    ComPtr<ITaskFolder> root;
    const TestBstr rootPath(L"\\");
    ASSERT_TRUE(SUCCEEDED(service->GetFolder(rootPath.get(), root.releaseAndGetAddressOf())));

    // The current user, as the probe names it: "DOMAIN\user" for the principal and the trigger.
    const std::vector<std::string> names = Platform::Windows::makeTaskSchedulerApi()->currentUserNames();
    ASSERT_GE(names.size(), 2U) << "the current user's names couldn't be read";
    const TestBstr user(WinString::utf8ToWide(names[1]));

    ComPtr<ITaskDefinition> definition;
    ASSERT_TRUE(SUCCEEDED(service->NewTask(0, definition.releaseAndGetAddressOf())));
    ComPtr<IPrincipal> principal;
    ASSERT_TRUE(SUCCEEDED(definition->get_Principal(principal.releaseAndGetAddressOf())));
    ASSERT_TRUE(SUCCEEDED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)));
    ASSERT_TRUE(SUCCEEDED(principal->put_UserId(user.get())));
    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logon;
    ASSERT_TRUE(SUCCEEDED(definition->get_Triggers(triggers.releaseAndGetAddressOf())));
    ASSERT_TRUE(SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, trigger.releaseAndGetAddressOf())));
    ASSERT_TRUE(SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.releaseAndGetAddressOf()))));
    ASSERT_TRUE(SUCCEEDED(logon->put_UserId(user.get())));
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    ASSERT_TRUE(SUCCEEDED(definition->get_Actions(actions.releaseAndGetAddressOf())));
    ASSERT_TRUE(SUCCEEDED(actions->Create(TASK_ACTION_EXEC, action.releaseAndGetAddressOf())));
    ASSERT_TRUE(SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.releaseAndGetAddressOf()))));
    const std::filesystem::path stub = std::filesystem::path(TASKSMACK_CI_STUB_PATH).make_preferred();
    const TestBstr stubPath(stub.wstring());
    const TestBstr stubArguments(L"1");
    ASSERT_TRUE(SUCCEEDED(exec->put_Path(stubPath.get())));
    ASSERT_TRUE(SUCCEEDED(exec->put_Arguments(stubArguments.get())));

    const std::wstring name = L"TaskSmackCiStartupTask" + TestSupport::CiRealSystem::uniqueSuffix();
    const TestBstr taskName(name);
    ComPtr<IRegisteredTask> registered;
    const HRESULT made = root->RegisterTaskDefinition(
        taskName.get(), definition.get(), TASK_CREATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN, none, registered.releaseAndGetAddressOf());
    ASSERT_TRUE(SUCCEEDED(made)) << "couldn't register the throwaway task: 0x" << std::hex << static_cast<unsigned long>(made);
    const TaskRemover removed(root, name); // never deletes anything but this task
    registered.reset();

    const std::wstring widePath = L"\\" + name;
    const std::string path = WinString::wideToUtf8(widePath);
    Platform::WindowsStartupProbe probe;
    const std::optional<Platform::StartupEntry> fresh = listedTask(probe, path);
    ASSERT_TRUE(fresh.has_value()) << "the probe doesn't list the throwaway logon task";
    const Platform::StartupEntry listed = fresh.value_or(Platform::StartupEntry{});
    EXPECT_EQ(listed.name, WinString::wideToUtf8(name));
    EXPECT_EQ(listed.location, Platform::StartupLocation::ScheduledTaskUser) << "it runs as the current user";
    EXPECT_EQ(listed.scope, Platform::StartupScope::User);
    EXPECT_TRUE(listed.enabled);
    EXPECT_FALSE(listed.osComponent);
    EXPECT_EQ(listed.target, Platform::StartupTargetState::Present) << listed.command;
    EXPECT_TRUE(Platform::hasSwitchableState(listed));

    Platform::WindowsStartupActions startupActions(false);
    const Platform::StartupActionResult disabled = startupActions.setEnabled(listed, false);
    ASSERT_TRUE(disabled.ok) << disabled.message;
    EXPECT_EQ(taskEnabled(root.get(), widePath), std::optional<bool>(false));
    const std::optional<Platform::StartupEntry> off = listedTask(probe, path);
    ASSERT_TRUE(off.has_value()) << "a disabled task is still listed";
    EXPECT_FALSE(off.value_or(Platform::StartupEntry{}).enabled) << "the switch is read again at the next sample";

    const Platform::StartupActionResult enabled = startupActions.setEnabled(off.value_or(listed), true);
    ASSERT_TRUE(enabled.ok) << enabled.message;
    EXPECT_EQ(taskEnabled(root.get(), widePath), std::optional<bool>(true));
    const std::optional<Platform::StartupEntry> on = listedTask(probe, path);
    ASSERT_TRUE(on.has_value());
    EXPECT_TRUE(on.value_or(Platform::StartupEntry{}).enabled);
}

} // namespace
