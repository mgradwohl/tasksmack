#pragma once

// Replace Task Manager's state on the UI thread (#3072): what the Settings rows and the startup notice
// show, and the changes they ask for, run on a worker of their own. The registry read and the change (a
// UAC prompt, then the helper's answer) block, so neither ever runs on the UI thread; the UI thread starts
// them and takes their result in, each frame, with poll().
//
// One controller is shared by SettingsLayer and TaskManagerNoticeLayer (main.cpp makes it). All of it is
// for the UI thread only; the worker shares nothing with it but the service and the result it returns.

#include "App/Panels/BackgroundTask.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace App
{

/// What the user asked for, from the Settings rows or the notice.
enum class TaskManagerRequest : std::uint8_t
{
    None,
    TurnOn,               ///< Point Task Manager at this copy; refused when another program has it
    TurnOnReplacingOther, ///< The same, replacing another program's Debugger: the user confirmed it
    TurnOff,              ///< Give Task Manager back
    Repair,               ///< Point a replacement that starts another TaskSmack at this copy
};

/// What a click on the Settings toggle asks for, given the state it shows: Off turns it on, either
/// TaskSmack state turns it off, and another program's Debugger needs the user's confirmation first
/// (TaskManagerRequest::None, with @c confirm set).
struct ToggleOutcome
{
    TaskManagerRequest request = TaskManagerRequest::None;
    bool confirm = false;
};

[[nodiscard]] constexpr ToggleOutcome toggleOutcome(Platform::TaskManagerReplacement::State state) noexcept
{
    using Platform::TaskManagerReplacement::State;
    switch (state)
    {
    case State::Off:
        return {.request = TaskManagerRequest::TurnOn, .confirm = false};
    case State::ThisCopy:
    case State::OtherTaskSmack:
        return {.request = TaskManagerRequest::TurnOff, .confirm = false};
    case State::OtherProgram:
        return {.request = TaskManagerRequest::None, .confirm = true};
    }
    return {};
}

/// What the views draw.
struct TaskManagerReplacementView
{
    /// The last read; nullopt until the first one is in.
    std::optional<Platform::TaskManagerReplacementInfo> info;
    /// The last change's result, until the next change starts.
    std::optional<Platform::ProcessActionResult> lastChange;
    /// How many reads have come in: the startup notice decides on the first.
    std::uint32_t readsCompleted = 0;
    /// There is something to replace Task Manager with: Windows, and not a synthetic run.
    bool supported = false;
    /// A read or a change runs.
    bool busy = false;
};

class TaskManagerReplacementController
{
  public:
    /// Over @p service; null (Linux, a synthetic run) makes an unsupported controller that does nothing.
    explicit TaskManagerReplacementController(std::shared_ptr<Platform::ITaskManagerReplacement> service);

    /// Starts a read, unless a read or change already runs.
    void refresh();
    /// Starts @p request's change, then a read, unless a read or change already runs.
    void request(TaskManagerRequest request);
    /// Takes a finished worker's result in. Each frame, on the UI thread.
    void poll();

    [[nodiscard]] const TaskManagerReplacementView& view() const noexcept
    {
        return m_View;
    }

    /// Waits for work in flight and takes its result in (tests).
    void waitForWork();

  private:
    struct Result
    {
        std::optional<Platform::ProcessActionResult> change;
        Platform::TaskManagerReplacementInfo info;
    };
    [[nodiscard]] static Result failed(std::string detail);
    void take(Result result);

    std::shared_ptr<Platform::ITaskManagerReplacement> m_Service;
    TaskManagerReplacementView m_View;
    BackgroundTask<Result> m_Worker; // last: destroyed (waited for) first, while the rest is alive
};

} // namespace App
