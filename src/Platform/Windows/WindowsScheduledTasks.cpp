#include "WindowsScheduledTasks.h"

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"

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
#include <sddl.h>
#include <taskschd.h>
// clang-format on

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")

#include "ComPtr.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsScheduledTasksMath.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

namespace Math = ScheduledTaskMath;

static_assert(Math::TRIGGER_BOOT == TASK_TRIGGER_BOOT);
static_assert(Math::TRIGGER_LOGON == TASK_TRIGGER_LOGON);
static_assert(static_cast<HRESULT>(Math::HRESULT_ACCESS_DENIED) == E_ACCESSDENIED);

/// Owns one BSTR: freed exactly once, on destruction or when it is written again.
class Bstr
{
  public:
    Bstr() noexcept = default;
    /// A copy of @p utf8; throws std::bad_alloc when it can't be allocated.
    explicit Bstr(std::string_view utf8) : m_Value(SysAllocString(WinString::utf8ToWide(utf8).c_str()))
    {
        if (m_Value == nullptr)
        {
            throw std::bad_alloc();
        }
    }
    ~Bstr()
    {
        SysFreeString(m_Value); // null is allowed
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    Bstr(Bstr&&) = delete;
    Bstr& operator=(Bstr&&) = delete;

    [[nodiscard]] BSTR get() const noexcept
    {
        return m_Value;
    }
    /// Frees the current string, for a call that writes a new one.
    [[nodiscard]] BSTR* put() noexcept
    {
        SysFreeString(m_Value);
        m_Value = nullptr;
        return &m_Value;
    }
    [[nodiscard]] std::string utf8() const
    {
        return m_Value == nullptr ? std::string() : WinString::wideToUtf8(std::wstring_view(m_Value, SysStringLen(m_Value)));
    }

  private:
    BSTR m_Value = nullptr;
};

/// Owns one VARIANT: cleared on destruction, whatever it holds.
class Variant
{
  public:
    Variant() noexcept
    {
        VariantInit(&m_Value);
    }
    /// A VT_I4 index, as the collections' get_Item takes.
    explicit Variant(LONG index) noexcept : Variant()
    {
        // VARIANT is the OS's tagged union: vt says which member holds the value.
        m_Value.vt = VT_I4;   // NOLINT(cppcoreguidelines-pro-type-union-access) - VARIANT's own tag
        m_Value.lVal = index; // NOLINT(cppcoreguidelines-pro-type-union-access) - the member VT_I4 names
    }
    ~Variant()
    {
        VariantClear(&m_Value);
    }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    Variant(Variant&&) = delete;
    Variant& operator=(Variant&&) = delete;

    /// A bitwise copy for a by-value VARIANT parameter; this one keeps ownership (callees don't free their copy).
    [[nodiscard]] VARIANT get() const noexcept
    {
        return m_Value;
    }

  private:
    VARIANT m_Value{};
};

/// Frees what the security calls allocate with LocalAlloc.
struct LocalFreer
{
    void operator()(void* memory) const noexcept
    {
        LocalFree(memory);
    }
};

/// A string property read through @p getter; empty when the call fails.
template<typename Interface, typename Getter> [[nodiscard]] std::string readString(Interface* object, Getter getter)
{
    Bstr value;
    if (object == nullptr || FAILED((object->*getter)(value.put())))
    {
        return {};
    }
    return value.utf8();
}

/// @p definition's triggers, in order.
[[nodiscard]] std::vector<Math::TaskTrigger> readTriggers(ITaskDefinition* definition)
{
    std::vector<Math::TaskTrigger> triggers;
    ComPtr<ITriggerCollection> collection;
    LONG count = 0;
    if (FAILED(definition->get_Triggers(collection.releaseAndGetAddressOf())) || !collection || FAILED(collection->get_Count(&count)))
    {
        return triggers;
    }
    for (LONG index = 1; index <= count; ++index) // the collections count from 1
    {
        ComPtr<ITrigger> trigger;
        TASK_TRIGGER_TYPE2 type = TASK_TRIGGER_EVENT;
        if (FAILED(collection->get_Item(index, trigger.releaseAndGetAddressOf())) || !trigger || FAILED(trigger->get_Type(&type)))
        {
            continue;
        }
        VARIANT_BOOL enabled = VARIANT_FALSE;
        const bool enabledRead = SUCCEEDED(trigger->get_Enabled(&enabled));
        // A trigger whose enabled state can't be read is skipped, as one whose type can't be.
        if (const auto read = Math::readTrigger(true, static_cast<int>(type), enabledRead, enabled != VARIANT_FALSE))
        {
            triggers.push_back(*read);
        }
    }
    return triggers;
}

/// The first exec action's Path and Arguments into @p record; nothing when the task has none (a COM handler's).
void readExecAction(ITaskDefinition* definition, Math::TaskRecord& record)
{
    ComPtr<IActionCollection> actions;
    LONG count = 0;
    if (FAILED(definition->get_Actions(actions.releaseAndGetAddressOf())) || !actions || FAILED(actions->get_Count(&count)))
    {
        return;
    }
    for (LONG index = 1; index <= count; ++index)
    {
        ComPtr<IAction> action;
        TASK_ACTION_TYPE type = TASK_ACTION_COM_HANDLER;
        if (FAILED(actions->get_Item(index, action.releaseAndGetAddressOf())) || !action || FAILED(action->get_Type(&type)) ||
            type != TASK_ACTION_EXEC)
        {
            continue;
        }
        ComPtr<IExecAction> exec;
        if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.releaseAndGetAddressOf()))) || !exec)
        {
            continue;
        }
        record.execPath = readString(exec.get(), &IExecAction::get_Path);
        record.execArguments = readString(exec.get(), &IExecAction::get_Arguments);
        record.workingDirectory = readString(exec.get(), &IExecAction::get_WorkingDirectory);
        return;
    }
}

/// What the Startup tab needs of @p task. Its action, author and principal are read only when it starts at sign-in
/// or boot: the walk's cost is mostly the definitions, and most tasks have other triggers. An enabled state that
/// can't be read is the HRESULT that says why: guessing it would let a switch skip the write it needs.
[[nodiscard]] TaskReading readRecord(IRegisteredTask* task)
{
    Math::TaskRecord record;
    record.path = readString(task, &IRegisteredTask::get_Path);
    VARIANT_BOOL enabled = VARIANT_TRUE;
    if (const HRESULT read = task->get_Enabled(&enabled); FAILED(read))
    {
        return std::unexpected(static_cast<std::int32_t>(read));
    }
    record.enabled = enabled != VARIANT_FALSE;

    ComPtr<ITaskDefinition> definition;
    if (FAILED(task->get_Definition(definition.releaseAndGetAddressOf())) || !definition)
    {
        return record; // no triggers read: not listed
    }
    record.triggers = readTriggers(definition.get());
    if (!Math::startsAtSignInOrBoot(record.triggers))
    {
        return record;
    }
    readExecAction(definition.get(), record);
    ComPtr<IRegistrationInfo> info;
    if (SUCCEEDED(definition->get_RegistrationInfo(info.releaseAndGetAddressOf())) && info)
    {
        record.author = readString(info.get(), &IRegistrationInfo::get_Author);
    }
    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(definition->get_Principal(principal.releaseAndGetAddressOf())) && principal)
    {
        record.userId = readString(principal.get(), &IPrincipal::get_UserId);
    }
    return record;
}

/// ITaskService and the folders it opens. Made, used and released on one thread inside the caller's ComScope.
class TaskSchedulerApi final : public ScheduledTaskApi
{
  public:
    [[nodiscard]] std::int32_t connect() override
    {
        HRESULT result = CoCreateInstance(CLSID_TaskScheduler,
                                          nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          IID_ITaskService,
                                          reinterpret_cast<void**>(m_Service.releaseAndGetAddressOf()));
        if (SUCCEEDED(result))
        {
            // The local machine, as the current user: four empty VARIANTs.
            const Variant none;
            result = m_Service->Connect(none.get(), none.get(), none.get(), none.get());
        }
        if (FAILED(result))
        {
            m_Service.reset();
        }
        return static_cast<std::int32_t>(result);
    }

    [[nodiscard]] std::vector<std::string> subfolders(const std::string& folderPath) override
    {
        std::vector<std::string> paths;
        const ComPtr<ITaskFolder> folder = open(folderPath);
        ComPtr<ITaskFolderCollection> collection;
        LONG count = 0;
        if (!folder || FAILED(folder->GetFolders(0, collection.releaseAndGetAddressOf())) || !collection ||
            FAILED(collection->get_Count(&count)))
        {
            return paths;
        }
        for (LONG index = 1; index <= count; ++index)
        {
            ComPtr<ITaskFolder> child;
            const Variant item(index);
            if (SUCCEEDED(collection->get_Item(item.get(), child.releaseAndGetAddressOf())) && child)
            {
                if (std::string path = readString(child.get(), &ITaskFolder::get_Path); !path.empty())
                {
                    paths.push_back(std::move(path));
                }
            }
        }
        return paths;
    }

    [[nodiscard]] std::vector<TaskReading> tasks(const std::string& folderPath) override
    {
        std::vector<TaskReading> records;
        const ComPtr<ITaskFolder> folder = open(folderPath);
        ComPtr<IRegisteredTaskCollection> collection;
        LONG count = 0;
        if (!folder || FAILED(folder->GetTasks(TASK_ENUM_HIDDEN, collection.releaseAndGetAddressOf())) || !collection ||
            FAILED(collection->get_Count(&count)))
        {
            return records;
        }
        for (LONG index = 1; index <= count; ++index)
        {
            ComPtr<IRegisteredTask> task;
            const Variant item(index);
            if (SUCCEEDED(collection->get_Item(item.get(), task.releaseAndGetAddressOf())) && task)
            {
                records.push_back(readRecord(task.get()));
            }
        }
        return records;
    }

    [[nodiscard]] TaskReading task(const std::string& path) override
    {
        ComPtr<IRegisteredTask> registered;
        if (const HRESULT result = find(path, registered); FAILED(result))
        {
            return std::unexpected(static_cast<std::int32_t>(result));
        }
        return readRecord(registered.get());
    }

    [[nodiscard]] std::int32_t setEnabled(const std::string& path, bool enabled) override
    {
        ComPtr<IRegisteredTask> registered;
        if (const HRESULT result = find(path, registered); FAILED(result))
        {
            return static_cast<std::int32_t>(result);
        }
        return static_cast<std::int32_t>(registered->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE));
    }

    [[nodiscard]] std::vector<std::string> currentUserNames() override
    {
        std::vector<std::string> names;
        UniqueHandle token;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
        {
            return names;
        }
        DWORD bytes = 0;
        static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes));
        if (bytes == 0)
        {
            return names;
        }
        std::vector<std::byte> buffer(bytes);
        if (GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes) == FALSE)
        {
            return names;
        }
        const PSID sid = reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid;
        LPWSTR sidText = nullptr;
        if (ConvertSidToStringSidW(sid, &sidText) != FALSE)
        {
            const std::unique_ptr<wchar_t, LocalFreer> owned(sidText);
            names.push_back(WinString::wideToUtf8(owned.get()));
        }
        std::array<wchar_t, 256> name{};
        std::array<wchar_t, 256> domain{};
        auto nameChars = static_cast<DWORD>(name.size());
        auto domainChars = static_cast<DWORD>(domain.size());
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountSidW(nullptr, sid, name.data(), &nameChars, domain.data(), &domainChars, &use) != FALSE)
        {
            const std::string user = WinString::wideToUtf8(name.data());
            names.push_back(WinString::wideToUtf8(domain.data()) + "\\" + user);
            names.push_back(user);
        }
        return names;
    }

  private:
    /// The folder at @p path; null when it can't be opened (not connected, gone, or denied).
    [[nodiscard]] ComPtr<ITaskFolder> open(const std::string& path) const
    {
        ComPtr<ITaskFolder> folder;
        if (m_Service)
        {
            const Bstr name(path);
            if (FAILED(m_Service->GetFolder(name.get(), folder.releaseAndGetAddressOf())))
            {
                folder.reset();
            }
        }
        return folder;
    }

    /// The task at full path @p path, from the root folder.
    [[nodiscard]] HRESULT find(const std::string& path, ComPtr<IRegisteredTask>& task) const
    {
        const ComPtr<ITaskFolder> root = open("\\");
        if (!root)
        {
            return E_FAIL;
        }
        const Bstr name(path);
        const HRESULT result = root->GetTask(name.get(), task.releaseAndGetAddressOf());
        if (SUCCEEDED(result) && !task)
        {
            return E_POINTER;
        }
        return result;
    }

    ComPtr<ITaskService> m_Service;
};

} // namespace

std::unique_ptr<ScheduledTaskApi> makeTaskSchedulerApi()
{
    return std::make_unique<TaskSchedulerApi>();
}

std::optional<std::vector<Math::TaskRecord>> readStartupTasks(ScheduledTaskApi& api)
{
    if (FAILED(api.connect()))
    {
        return std::nullopt;
    }
    std::vector<Math::TaskRecord> found;
    std::vector<std::pair<std::string, int>> pending{{"\\", 0}};
    while (!pending.empty())
    {
        auto [folder, depth] = std::move(pending.back());
        pending.pop_back();
        for (TaskReading& task : api.tasks(folder))
        {
            // A task whose state can't be read is left out: listed, it would show a state nobody read.
            if (task.has_value() && Math::startsAtSignInOrBoot(task->triggers))
            {
                found.push_back(std::move(*task));
            }
        }
        if (depth < Math::MAX_FOLDER_DEPTH)
        {
            for (std::string& child : api.subfolders(folder))
            {
                pending.emplace_back(std::move(child), depth + 1);
            }
        }
    }
    return found;
}

StartupActionResult setStartupTaskEnabled(ScheduledTaskApi& api, const StartupEntry& entry, bool enabled)
{
    if (entry.location != StartupLocation::ScheduledTaskUser && entry.location != StartupLocation::ScheduledTaskMachine)
    {
        return StartupActionResult::failed("Not a scheduled task");
    }
    const std::string& path = entry.registrationName;
    if (path.empty())
    {
        return StartupActionResult::failed("The scheduled task has no path to switch it by");
    }
    if (Math::isMicrosoftTask(path))
    {
        return StartupActionResult::failed(std::string(Math::MICROSOFT_TASK_REASON));
    }
    if (const std::int32_t connected = api.connect(); FAILED(connected))
    {
        return StartupActionResult::failed(Math::taskErrorText(static_cast<std::uint32_t>(connected)));
    }
    // Read again here: it may have gone, or been switched elsewhere, since the list was read.
    // Its enabled state too: one that can't be read fails the switch, rather than skip a write it may need.
    const TaskReading current = api.task(path);
    if (!current.has_value())
    {
        return StartupActionResult::failed(Math::taskErrorText(static_cast<std::uint32_t>(current.error())));
    }
    if (current->enabled != enabled)
    {
        if (const std::int32_t written = api.setEnabled(path, enabled); FAILED(written))
        {
            return StartupActionResult::failed(Math::taskErrorText(static_cast<std::uint32_t>(written)));
        }
    }
    // Counted even when it was already so: the list that said otherwise is out of date.
    scheduledTaskChanges().fetch_add(1, std::memory_order_relaxed);
    return StartupActionResult::succeeded();
}

std::atomic<std::uint64_t>& scheduledTaskChanges() noexcept
{
    static std::atomic<std::uint64_t> changes{0};
    return changes;
}

} // namespace Platform::Windows
