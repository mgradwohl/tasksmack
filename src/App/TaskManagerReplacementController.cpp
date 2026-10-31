#include "TaskManagerReplacementController.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/ThreadName.h"

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace App
{

TaskManagerReplacementController::TaskManagerReplacementController(std::shared_ptr<Platform::ITaskManagerReplacement> service)
    : m_Service(std::move(service)), m_Worker(Platform::TASK_MANAGER_THREAD_NAME, &TaskManagerReplacementController::failed)
{
    m_View.supported = m_Service != nullptr;
}

TaskManagerReplacementController::Result TaskManagerReplacementController::failed(std::string detail)
{
    // No thread for the work, or the work threw: shown as a read that failed, with why.
    Result result;
    result.info.readFailed = true;
    result.info.readError = std::move(detail);
    return result;
}

void TaskManagerReplacementController::refresh()
{
    if (m_Service == nullptr || m_Worker.busy())
    {
        return;
    }
    if (auto failedToStart = m_Worker.start([service = m_Service] { return Result{.change = std::nullopt, .info = service->read()}; }))
    {
        take(std::move(*failedToStart));
        return;
    }
    m_View.busy = true;
}

void TaskManagerReplacementController::request(TaskManagerRequest request)
{
    if (m_Service == nullptr || m_Worker.busy() || request == TaskManagerRequest::None)
    {
        return;
    }
    const auto change =
        request == TaskManagerRequest::TurnOff ? Platform::Broker::TaskManagerChange::Clear : Platform::Broker::TaskManagerChange::Set;
    const bool replaceOther = request == TaskManagerRequest::TurnOnReplacingOther;
    m_View.lastChange.reset();
    if (auto failedToStart = m_Worker.start(
            [service = m_Service, change, replaceOther](const std::stop_token& stopToken)
            {
                Result result;
                result.change = service->change(change, replaceOther, stopToken);
                result.info = service->read(); // what it is now, whatever the change did
                return result;
            }))
    {
        take(std::move(*failedToStart));
        return;
    }
    m_View.busy = true;
}

void TaskManagerReplacementController::poll()
{
    if (auto result = m_Worker.poll())
    {
        take(std::move(*result));
    }
}

void TaskManagerReplacementController::waitForWork()
{
    if (auto result = m_Worker.poll(true))
    {
        take(std::move(*result));
    }
}

void TaskManagerReplacementController::take(Result result)
{
    m_View.busy = false;
    m_View.info = std::move(result.info);
    ++m_View.readsCompleted;
    if (result.change.has_value())
    {
        m_View.lastChange = std::move(result.change);
    }
}

} // namespace App
