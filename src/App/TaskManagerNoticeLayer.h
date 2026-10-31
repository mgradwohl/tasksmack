#pragma once

#include "App/TaskManagerReplacementController.h"
#include "App/TaskManagerReplacementRows.h"
#include "Core/Layer.h"

#include <memory>

namespace App
{

/// Replace Task Manager's startup check (#3072): at attach it reads taskmgr.exe's IFEO Debugger on the
/// controller's worker, and when that first read says Task Manager starts a TaskSmack that isn't this one
/// and should (TaskManagerReplacementText::showsNotice()), it shows a notice that blocks nothing, offering
/// Repair (point it at this copy) or Turn off. Asked once per run.
///
/// Thread safety: all layer lifecycle methods run on the main thread; the controller's worker shares
/// only its result.
class TaskManagerNoticeLayer : public Core::Layer
{
  public:
    explicit TaskManagerNoticeLayer(std::shared_ptr<TaskManagerReplacementController> controller);

    void onAttach() override;
    void onRender() override;

    [[nodiscard]] const TaskManagerReplacementRows::NoticeState& noticeState() const noexcept
    {
        return m_Notice;
    }

  private:
    std::shared_ptr<TaskManagerReplacementController> m_Controller;
    TaskManagerReplacementRows::NoticeState m_Notice;
    bool m_Decided = false; ///< The first read came in and the notice was shown, or not
};

} // namespace App
