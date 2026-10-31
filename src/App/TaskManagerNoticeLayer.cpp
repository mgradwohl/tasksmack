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

void TaskManagerNoticeLayer::onRender()
{
    if (m_Controller == nullptr)
    {
        return;
    }
    m_Controller->poll();
    const TaskManagerReplacementView& view = m_Controller->view();
    if (!m_Decided && view.readsCompleted > 0)
    {
        m_Decided = true;
        m_Notice.open = TaskManagerReplacementText::showsNotice(view);
        if (m_Notice.open && view.info.has_value())
        {
            spdlog::info("Task Manager starts another TaskSmack ({}): offering to repair it", view.info->status.program);
        }
    }
    const TaskManagerRequest request = TaskManagerReplacementRows::renderNotice(view, m_Notice);
    m_Controller->request(request);
}

} // namespace App
