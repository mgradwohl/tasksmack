#include "BrokeredUdpTrafficCollector.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/UdpByteCounts.h"

#include <spdlog/spdlog.h>

#include <cstdint>
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
    // Asked before the request: a failed or dismissed restart of the session's helper leaves it disconnected, which
    // afterwards would read as no session at all.
    const bool held = m_Broker->heldForSession();
    const std::stop_token never; // a prompt, if any, is the user's to answer or dismiss
    const auto counting = m_Broker->countUdpBytes(never);
    if (!counting.has_value() && held)
    {
        setProblem(counting.error());
        return UdpCollectorStartResult::Failed; // stays stopped, so the next start() tries again
    }
    {
        const std::scoped_lock lock(m_Mutex);
        // A new run: whatever a read of an earlier one brings back is dropped, and its first totals are the baseline,
        // as a helper that kept counting while this was stopped would otherwise have them counted again.
        ++m_Run;
        m_NeedBaseline = true;
        m_Baseline.clear();
        m_Last.clear();
        m_Problem = counting.has_value() ? std::string{} : counting.error();
        m_Running = true;
    }
    if (!counting.has_value())
    {
        // No session yet: the request stands, so the session's helper counts once one is chosen, and sample() reads it
        // from then on. Until then each reading finds no counting helper and adds nothing.
        return UdpCollectorStartResult::NotElevated;
    }
    spdlog::info("UDP traffic per process: counting through the helper");
    return UdpCollectorStartResult::Started;
}

void BrokeredUdpTrafficCollector::stop()
{
    {
        const std::scoped_lock lock(m_Mutex);
        ++m_Run; // a read in flight publishes nothing now
        m_Running = false;
        m_Delivering = false;
        m_Last.clear();
        m_Baseline.clear();
    }
    m_Broker->stopCountingUdpBytes();
}

bool BrokeredUdpTrafficCollector::isDelivering() const
{
    return m_Delivering.load();
}

std::vector<UdpProcessTraffic> BrokeredUdpTrafficCollector::sample()
{
    std::uint64_t run = 0;
    {
        const std::scoped_lock lock(m_Mutex);
        if (!m_Running.load())
        {
            return {};
        }
        run = m_Run;
    }
    const auto response = m_Broker->callIfIdle(Broker::UdpBytesRequest{}, SAMPLE_TIMEOUT);
    const auto* const reply = response.has_value() ? std::get_if<Broker::UdpBytesReply>(&*response) : nullptr;
    const bool helperGone = reply == nullptr && !m_Broker->connected();
    const std::scoped_lock lock(m_Mutex);
    if (run != m_Run || !m_Running.load())
    {
        return {}; // stopped, or stopped and started again, while this read was out
    }
    if (reply == nullptr && !helperGone)
    {
        // Busy or slow: the counters only grow, so the last totals stand for this reading.
        return m_Delivering.load() ? m_Last : std::vector<UdpProcessTraffic>{};
    }
    if (reply == nullptr || reply->status != Broker::UdpBytesStatus::Ok)
    {
        // A helper that ended took its counter with it, and the next one counts from zero (taken below as a reset).
        std::string problem;
        if (helperGone)
        {
            problem = "The helper ended";
        }
        else if (reply->status == Broker::UdpBytesStatus::NotCounting)
        {
            problem = "The helper isn't counting UDP";
        }
        else
        {
            problem = reply->detail;
        }
        if (problem != m_Problem)
        {
            // Once per change, not per reading.
            spdlog::warn("UDP traffic per process: {}; the counters stay TCP only", problem);
        }
        m_Problem = std::move(problem);
        m_Delivering = false;
        m_Last.clear();
        return {};
    }
    if (m_NeedBaseline)
    {
        // The run's first totals: from here on only what's counted after them.
        m_Baseline.clear();
        for (const UdpByteCounts& process : reply->processes)
        {
            m_Baseline[process.tgid] = {process.txBytes, process.rxBytes};
        }
        m_NeedBaseline = false;
    }
    m_Last.clear();
    m_Last.reserve(reply->processes.size());
    for (const UdpByteCounts& process : reply->processes)
    {
        auto [sentBase, receivedBase] = m_Baseline[process.tgid];
        if (process.txBytes < sentBase || process.rxBytes < receivedBase)
        {
            // Below its baseline: a new helper's counter, from zero. Its totals are new bytes.
            sentBase = 0;
            receivedBase = 0;
            m_Baseline[process.tgid] = {0, 0};
        }
        m_Last.push_back({.pid = process.tgid, .bytesSent = process.txBytes - sentBase, .bytesReceived = process.rxBytes - receivedBase});
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
