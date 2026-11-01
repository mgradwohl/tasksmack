#include "TaskManagerNoticeLayer.h"

#include "App/TaskManagerReplacementController.h"
#include "App/TaskManagerReplacementRows.h"
#include "App/TaskManagerReplacementText.h"
#include "Core/Layer.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace App
{

TaskManagerNoticeLayer::TaskManagerNoticeLayer(std::shared_ptr<TaskManagerReplacementController> controller)
    : Core::Layer("TaskManagerNoticeLayer"), m_Controller(std::move(controller))
{}

void TaskManagerNoticeLayer::onAttach()
{
    if (m_Controller != nullptr)
    {
        m_Controller->refresh(); // the registry read, off the UI thread
    }
}

void TaskManagerNoticeLayer::onHiddenUpdate()
{
    if (m_Controller != nullptr)
    {
        m_Controller->poll();
    }
}

void TaskManagerNoticeLayer::onRender()
{
    if (m_Controller == nullptr)
    {
        return;
    }
    m_Controller->poll();
    const TaskManagerReplacementView& view = m_Controller->view();
    // Decided on the first read, once the uninstall mark was checked (#3276): after an uninstall nothing is asked.
    if (!m_Decided && view.readsCompleted > 0 && view.intentChecked)
    {
        m_Decided = true;
        m_Notice.kind = TaskManagerReplacementText::noticeKind(view);
        m_Notice.open = m_Notice.kind != NoticeKind::None;
        if (m_Notice.kind == NoticeKind::Repair && view.info.has_value())
        {
            spdlog::info("Task Manager starts another TaskSmack ({}): offering to repair it", view.info->status.program);
        }
        else if (m_Notice.kind == NoticeKind::Restore)
        {
            spdlog::info("Replace Task Manager was on, and something turned it off: offering to turn it back on");
        }
    }
    // The restore notice goes once there is nothing to restore (#3276): turned on from Settings meanwhile, or
    // forgotten. Not one that was acted on, which shows its result.
    if (m_Notice.open && !m_Notice.acted && m_Notice.kind == NoticeKind::Restore && !TaskManagerReplacementText::showsRestoreNotice(view) &&
        !view.busy)
    {
        m_Notice.open = false;
    }
    const TaskManagerRequest request = TaskManagerReplacementRows::renderNotice(view, m_Notice);
    m_Controller->request(request);
}

} // namespace App
