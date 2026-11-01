#include "TaskManagerReplacementService.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/TaskManagerReplacement.h"

#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace Platform
{

TaskManagerReplacementService::TaskManagerReplacementService(TaskManagerReplacementSetup setup) : m_Setup(std::move(setup))
{}

TaskManagerReplacementInfo TaskManagerReplacementService::read()
{
    TaskManagerReplacementInfo info{
        .status = {},
        .readError = {},
        .selfPath = m_Setup.selfPath,
        .unavailableReason = m_Setup.unavailableReason,
        .turnOnUnavailableReason = {},
        .readFailed = false,
        .otherProgramExists = true,
        .installedCopy = m_Setup.installedCopy,
    };
    if (const auto refused = directSetRefused(); refused.has_value())
    {
        info.turnOnUnavailableReason = *refused;
    }
    if (!m_Setup.readDebugger)
    {
        info.readFailed = true;
        info.readError = "nothing reads Task Manager's setting here";
        return info;
    }
    const auto debugger = m_Setup.readDebugger();
    if (!debugger.has_value())
    {
        info.readFailed = true;
        info.readError = debugger.error();
        return info;
    }
    info.status = TaskManagerReplacement::classify(*debugger, m_Setup.selfPath);
    const bool startsAnother = info.status.state == TaskManagerReplacement::State::OtherTaskSmack ||
                               info.status.state == TaskManagerReplacement::State::OtherProgram;
    if (startsAnother && m_Setup.fileExists)
    {
        info.otherProgramExists = m_Setup.fileExists(info.status.program);
    }
    return info;
}

ProcessActionResult
TaskManagerReplacementService::change(Broker::TaskManagerChange change, bool replaceOther, const std::stop_token& stopToken)
{
    Broker::TaskManagerReplacementRequest request{.change = change, .path = {}, .replaceOther = false};
    if (change == Broker::TaskManagerChange::Set)
    {
        if (const auto valid = TaskManagerReplacement::validateTaskSmackPath(m_Setup.selfPath); !valid.has_value())
        {
            return ProcessActionResult::error("This TaskSmack can't replace Task Manager: " + std::string(valid.error()));
        }
        request.path = m_Setup.selfPath;
        request.replaceOther = replaceOther;
    }
    if (m_Setup.direct != nullptr)
    {
        if (change != Broker::TaskManagerChange::Set)
        {
            // Always allowed: applyClear() removes only a TaskSmack's Debugger.
            return TaskManagerReplacement::applyClear(*m_Setup.direct);
        }
        // Checked again here, just before the write, not trusted from the last read.
        if (const auto refused = directSetRefused(); refused.has_value())
        {
            return ProcessActionResult::error(*refused);
        }
        return TaskManagerReplacement::applySet(*m_Setup.direct, request.path, replaceOther);
    }
    if (m_Setup.broker != nullptr)
    {
        return changeThroughHelper(request, stopToken);
    }
    return ProcessActionResult::error(m_Setup.unavailableReason.empty() ? std::string("Replacing Task Manager needs administrator rights")
                                                                        : m_Setup.unavailableReason);
}

std::optional<std::string> TaskManagerReplacementService::directSetRefused() const
{
    if (m_Setup.direct == nullptr || !m_Setup.checkDirectSet)
    {
        return std::nullopt;
    }
    if (const auto allowed = m_Setup.checkDirectSet(); allowed.has_value())
    {
        return std::nullopt;
    }
    return m_Setup.directSetRefusal.empty() ? std::string("This copy of TaskSmack can't replace Task Manager") : m_Setup.directSetRefusal;
}

ProcessActionResult TaskManagerReplacementService::changeThroughHelper(const Broker::TaskManagerReplacementRequest& request,
                                                                       const std::stop_token& stopToken) const
{
    if (!m_Setup.broker->connected())
    {
        // The UAC prompt: blocking until it is answered, which is why this runs on a worker.
        if (const std::expected<void, std::string> connected = m_Setup.broker->connect(stopToken); !connected.has_value())
        {
            return ProcessActionResult::error("Couldn't act as an administrator: " + connected.error());
        }
    }
    return actionResultOf(m_Setup.broker->call(request, stopToken));
}

} // namespace Platform
