#include "BrokeredUdpTrafficCollector.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/UdpByteCounts.h"

#include <spdlog/spdlog.h>

#include <expected>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{

BrokeredUdpTrafficCollector::BrokeredUdpTrafficCollector(std::shared_ptr<IPrivilegedBroker> broker) : m_Broker(std::move(broker))
{}

UdpCollectorStartResult BrokeredUdpTrafficCollector::start()
{
    if (m_Running.load())
    {
        return UdpCollectorStartResult::Started;
    }
    const std::stop_token never; // a prompt, if any, is the user's to answer or dismiss
    const auto counting = m_Broker->countUdpBytes(never);
    if (!counting.has_value())
    {
        setProblem(counting.error());
        if (m_Broker->heldForSession())
        {
            return UdpCollectorStartResult::Failed; // the session's helper couldn't be restarted with the counter
        }
        // No session yet: the request stands, so the session's helper counts once one is chosen, and sample() reads it
        // from then on. Until then each reading finds no counting helper and adds nothing.
        m_Running = true;
        return UdpCollectorStartResult::NotElevated;
    }
    setProblem({});
    m_Running = true;
    spdlog::info("UDP traffic per process: counting through the helper");
    return UdpCollectorStartResult::Started;
}

void BrokeredUdpTrafficCollector::stop()
{
    m_Running = false;
    m_Delivering = false;
    m_Broker->stopCountingUdpBytes();
    const std::scoped_lock lock(m_Mutex);
    m_Last.clear();
}

bool BrokeredUdpTrafficCollector::isDelivering() const
{
    return m_Delivering.load();
}

std::vector<UdpProcessTraffic> BrokeredUdpTrafficCollector::sample()
{
    if (!m_Running.load())
    {
        return {};
    }
    const auto response = m_Broker->callIfIdle(Broker::UdpBytesRequest{}, SAMPLE_TIMEOUT);
    const auto* const reply = response.has_value() ? std::get_if<Broker::UdpBytesReply>(&*response) : nullptr;
    const std::scoped_lock lock(m_Mutex);
    if (reply == nullptr)
    {
        // Busy, slow or gone: the counters only grow, so the last totals stand for this reading. A helper that ended
        // takes its counter with it; the next one starts from zero, which Domain takes as new connections.
        return m_Delivering.load() ? m_Last : std::vector<UdpProcessTraffic>{};
    }
    if (reply->status != Broker::UdpBytesStatus::Ok)
    {
        m_Delivering = false;
        m_Last.clear();
        m_Problem = reply->status == Broker::UdpBytesStatus::NotCounting ? std::string("The helper isn't counting UDP") : reply->detail;
        return {};
    }
    m_Last.clear();
    m_Last.reserve(reply->processes.size());
    for (const UdpByteCounts& process : reply->processes)
    {
        m_Last.push_back({.pid = process.tgid, .bytesSent = process.txBytes, .bytesReceived = process.rxBytes});
    }
    m_Delivering = true;
    m_Problem.clear();
    return m_Last;
}

std::string BrokeredUdpTrafficCollector::problem() const
{
    const std::scoped_lock lock(m_Mutex);
    return m_Problem;
}

void BrokeredUdpTrafficCollector::setProblem(std::string problem)
{
    const std::scoped_lock lock(m_Mutex);
    m_Problem = std::move(problem);
}

std::shared_ptr<IUdpTrafficCollector> withPrivilegedBroker(std::shared_ptr<IUdpTrafficCollector> collector,
                                                           std::shared_ptr<IPrivilegedBroker> broker)
{
    if (collector != nullptr || broker == nullptr)
    {
        return collector;
    }
    return std::make_shared<BrokeredUdpTrafficCollector>(std::move(broker));
}

} // namespace Platform
