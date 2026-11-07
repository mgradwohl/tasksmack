#include "AdministratorHold.h"

#include "Domain/Worker.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/ThreadName.h"

#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace App
{

AdministratorHold::AdministratorHold()
    : m_Worker({.threadName = Platform::ADMIN_HOLD_THREAD_NAME, .capacity = 1, .admission = Domain::Admission::Refuse},
               [](HoldRequest& job, const std::stop_token& stop, HoldWorker::JobContext& context)
               {
                   // An exception (no memory) is the Worker's to contain: it ends Failed, which take() reports.
                   context.finish(HoldAnswer{.result = job.broker->holdForSession(stop)});
               })
{}

AdministratorHold::Result AdministratorHold::start(std::shared_ptr<Platform::IPrivilegedBroker> broker)
{
    if (broker == nullptr || m_Pending)
    {
        return std::unexpected(std::string(NOT_INSTALLED));
    }
    // Queued means the body runs (#3472): the job's stop state was made here, and only stop() drops it.
    switch (m_Worker.submit(HoldRequest{.broker = std::move(broker)}))
    {
    case Domain::SubmitResult::Queued:
        m_Pending = true;
        return {};
    case Domain::SubmitResult::Closed: // no thread could be started, or an earlier hold's body threw
    {
        std::string reason = m_Worker.failure();
        return std::unexpected(reason.empty() ? std::string(NOT_QUEUED) : std::move(reason));
    }
    default: // Refused: no memory to queue it (nothing else is queued, so the queue isn't full)
        return std::unexpected(std::string(NOT_QUEUED));
    }
}

std::optional<AdministratorHold::Result> AdministratorHold::take()
{
    if (!m_Pending)
    {
        return std::nullopt;
    }
    // Each copy below is made before the hold counts as taken, so one that throws (no memory) leaves it to take
    // again next frame.
    if (m_Worker.publishedVersion() != m_TakenVersion)
    {
        const auto published = m_Worker.latest();
        Result answer = published->value.result;
        m_TakenVersion = published->version;
        m_Pending = false;
        return answer;
    }
    // Read after the publication: a body that threw published nothing, and its worker is Failed for good.
    if (m_Worker.state() == Domain::WorkerState::Failed)
    {
        Result failed = std::unexpected(m_Worker.failure());
        m_Pending = false;
        return failed;
    }
    return std::nullopt;
}

} // namespace App
