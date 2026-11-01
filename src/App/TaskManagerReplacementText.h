#pragma once

// Replace Task Manager's words (#3072): the Settings rows' and the startup notice's, kept apart from
// their ImGui drawing (TaskManagerReplacementRows.cpp) so the tests check them on every platform.

#include "App/TaskManagerReplacementController.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <string>
#include <string_view>

namespace App::TaskManagerReplacementText
{

inline constexpr const char* TOGGLE_LABEL = "Replace Task Manager";
inline constexpr std::string_view NOTE = "Ctrl+Shift+Esc, and anything else that starts Task Manager, opens TaskSmack instead. "
                                         "Needs administrator rights: Windows asks first. It takes effect at once, without Save.";
inline constexpr std::string_view PORTABLE_WARNING =
    "This copy isn't installed in Program Files. Turn this off before deleting this copy, or Task Manager won't open.";
inline constexpr std::string_view BUSY = "Working...";
inline constexpr const char* NOTICE_TITLE = "Task Manager";
inline constexpr const char* REPAIR_LABEL = "Repair";
inline constexpr const char* TURN_OFF_LABEL = "Turn off";
inline constexpr const char* NOT_NOW_LABEL = "Not now";
inline constexpr const char* CLOSE_LABEL = "Close";
inline constexpr const char* REPLACE_LABEL = "Replace it";
inline constexpr const char* CANCEL_LABEL = "Cancel";

/// Whether the toggle shows as on: Task Manager starts a TaskSmack, this one or another.
[[nodiscard]] constexpr bool toggleChecked(Platform::TaskManagerReplacement::State state) noexcept
{
    return state == Platform::TaskManagerReplacement::State::ThisCopy || state == Platform::TaskManagerReplacement::State::OtherTaskSmack;
}

/// The state line under the toggle: Off, On (this copy), or what else Task Manager starts.
[[nodiscard]] inline std::string stateText(const TaskManagerReplacementView& view)
{
    if (!view.info.has_value())
    {
        return "Status: checking...";
    }
    const Platform::TaskManagerReplacementInfo& info = *view.info;
    if (info.readFailed)
    {
        return "Status: unknown (" + info.readError + ")";
    }
    using Platform::TaskManagerReplacement::State;
    switch (info.status.state)
    {
    case State::Off:
        return "Status: Off";
    case State::ThisCopy:
        return "Status: On (this copy)";
    case State::OtherTaskSmack:
        return "Status: points to another TaskSmack, " + info.status.program +
               (info.otherProgramExists ? "" : ", which is no longer there");
    case State::OtherProgram:
        return "Status: points to another program, " + info.status.program;
    }
    return {};
}

/// The confirmation a click on the toggle asks for while another program has Task Manager.
[[nodiscard]] inline std::string confirmText(std::string_view program)
{
    return "Task Manager is already replaced by " + std::string(program) +
           ". Replace it with TaskSmack? Turning this off later gives Task Manager back; it doesn't put that program back.";
}

/// Whether the startup check shows its notice (TaskManagerReplacement::offersRepair()), on a read that
/// came in.
[[nodiscard]] inline bool showsNotice(const TaskManagerReplacementView& view)
{
    return view.supported && view.info.has_value() && !view.info->readFailed &&
           Platform::TaskManagerReplacement::offersRepair(view.info->status.state, view.info->otherProgramExists, view.info->installedCopy);
}

/// Whether the startup check offers to turn it back on (#3276): the user turned it on, as config.toml
/// remembers, and a read finds it off, on the installed copy. An upgrade to another folder, an install of a
/// build whose uninstaller removed it, or another tool turned it off; asked rather than done, since turning it
/// on needs administrator rights. A portable copy isn't asked: the installed copy is the one it was for.
[[nodiscard]] inline bool showsRestoreNotice(const TaskManagerReplacementView& view)
{
    return view.supported && view.intended && view.info.has_value() && !view.info->readFailed && view.info->installedCopy &&
           view.info->status.state == Platform::TaskManagerReplacement::State::Off;
}

/// Which notice the startup check shows, on a read that came in: the repair, the restore, or none.
[[nodiscard]] inline NoticeKind noticeKind(const TaskManagerReplacementView& view)
{
    if (showsNotice(view))
    {
        return NoticeKind::Repair;
    }
    return showsRestoreNotice(view) ? NoticeKind::Restore : NoticeKind::None;
}

/// The startup notice's text.
[[nodiscard]] inline std::string noticeText(const Platform::TaskManagerReplacementInfo& info)
{
    std::string text = "Task Manager opens a TaskSmack that isn't this one: " + info.status.program;
    text += info.otherProgramExists ? "." : ", which is no longer there, so Task Manager doesn't open at all.";
    text += " Repair points it at this copy; Turn off gives Task Manager back. Either needs administrator rights.";
    return text;
}

/// The restore notice's text (#3276).
inline constexpr std::string_view RESTORE_NOTICE_TEXT =
    "Replace Task Manager was turned off outside TaskSmack, by an upgrade or another tool, so Task Manager opens again. "
    "Repair turns it back on for this copy, and needs administrator rights; Turn off leaves it off.";

} // namespace App::TaskManagerReplacementText
