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
        // A new run: whatever a read of an earlier one brings back is dropped, its counters are another generation (new
        // connections to Domain), and its first reading only sets their baseline, as a helper that kept counting while
        // this was stopped has totals from before.
        ++m_Run;
        ++m_Generation;
        m_Session = m_Broker->sessionsStarted();
        m_Seeding = true;
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
    // The helper before and after the read: one replaced meanwhile (another worker reconnected it) leaves the reply's
    // counter unknown, the old helper's or the new one's, so it's dropped.
    const std::uint64_t sessionBefore = m_Broker->sessionsStarted();
    const auto response = m_Broker->callIfIdle(Broker::UdpBytesRequest{}, SAMPLE_TIMEOUT);
    const auto* const reply = response.has_value() ? std::get_if<Broker::UdpBytesReply>(&*response) : nullptr;
    const bool helperGone = reply == nullptr && !m_Broker->connected();
    const std::uint64_t session = m_Broker->sessionsStarted();
    const std::scoped_lock lock(m_Mutex);
    if (run != m_Run || !m_Running.load())
    {
        return {}; // stopped, or stopped and started again, while this read was out
    }
    if (session != sessionBefore)
    {
        // Domain takes the empty reading as the old helper's connections closed; the next reading is the new helper's,
        // another generation, counted from zero.
        m_Session = session;
        ++m_Generation;
        m_Seeding = false;
        m_Delivering = false;
        m_Last.clear();
        return {};
    }
    if (session != m_Session)
    {
        // Another helper since the last reading (another reader may have restarted it): its counter started from zero,
        // so its totals are another generation, all of them new bytes.
        m_Session = session;
        ++m_Generation;
        m_Seeding = false;
    }
    if (reply == nullptr && !helperGone)
    {
        // Busy or slow: the last totals again, unreadable, so Domain keeps the connections open and re-baselines them
        // on the next reading rather than taking this interval for no traffic and the next for two.
        std::vector<UdpProcessTraffic> last = m_Last;
        for (UdpProcessTraffic& process : last)
        {
            process.readable = false;
        }
        return last;
    }
    if (reply == nullptr || reply->status != Broker::UdpBytesStatus::Ok)
    {
        // Domain takes this empty reading as every UDP connection closed. A helper that ended took its counter with it,
        // and its successor's (counting from zero, another generation above) are new. One that couldn't read its map or
        // isn't counting may still be counting: its next reading only seeds the baseline again. "The helper ended" is
        // only for a helper that delivered: before the first one, the reason start() gave stands (such as "Needs an
        // Administrator session").
        const bool hadHelper = m_Delivering.load();
        std::string problem;
        if (helperGone)
        {
            problem = hadHelper ? std::string("The helper ended") : m_Problem;
        }
        else
        {
            m_Seeding = true;
            problem = reply->status == Broker::UdpBytesStatus::NotCounting ? std::string("The helper isn't counting UDP") : reply->detail;
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
    // The helper's own cumulative totals: Domain compares them reading by reading. A process missing from one (its map
    // entry aged out) is a connection that closed, and its return a new one counted from its first bytes.
    const bool seeding = m_Seeding;
    m_Seeding = false;
    m_Last.clear();
    m_Last.reserve(reply->processes.size());
    for (const UdpByteCounts& process : reply->processes)
    {
        m_Last.push_back({
            .pid = process.tgid,
            .bytesSent = process.txBytes,
            .bytesReceived = process.rxBytes,
            .generation = m_Generation,
            .readable = true,
        });
    }
    m_Delivering = true;
    m_Problem.clear();
    std::vector<UdpProcessTraffic> traffic = m_Last;
    if (seeding)
    {
        for (UdpProcessTraffic& process : traffic)
        {
            process.baselineOnly = true; // the baseline: they may hold bytes from before this run
        }
    }
    return traffic;
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
