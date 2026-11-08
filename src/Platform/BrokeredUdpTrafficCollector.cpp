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
    {
        const std::scoped_lock lock(m_Mutex);
        if (m_Running.load() && m_Problem.empty())
        {
            return UdpCollectorStartResult::Started; // already counting, or asked with nothing in the way
        }
    }
    // Whether the session is held, as it is now: connected() looks at the channel first (this is a worker), so a held
    // helper that has exited unnoticed isn't taken for a session. Read before the request, as a failed or dismissed
    // restart of the session's helper leaves it disconnected, which afterwards would read as no session at all.
    static_cast<void>(m_Broker->connected());
    const bool held = m_Broker->heldForSession();
    const std::stop_token never; // a prompt, if any, is the user's to answer or dismiss
    const auto counting = m_Broker->countUdpBytes(never);
    std::uint64_t startVersion = 0;
    {
        const std::scoped_lock lock(m_Mutex);
        // A new run: whatever a read of an earlier one brings back is dropped, its counters are another generation (new
        // connections to Domain), and its first reading only sets their baseline, as a helper that kept counting while
        // this was stopped has totals from before. Armed even when the request failed: it stands in the broker, so the
        // session's next helper counts, and sample() delivers from then on whenever that is.
        ++m_Run;
        ++m_Generation;
        m_Session = m_Broker->sessionsStarted();
        m_Seeding = true;
        m_Last.clear();
        m_Problem = counting.has_value() ? std::string{} : counting.error();
        m_Running = true;
        startVersion = ++m_ProblemVersion;
    }
    publishProblem(counting.has_value() ? std::string{} : counting.error(), startVersion);
    if (counting.has_value())
    {
        spdlog::info("UDP traffic per process: counting through the helper");
        return UdpCollectorStartResult::Started;
    }
    spdlog::info("UDP traffic per process: {}; counting once a session's helper counts", counting.error());
    // A held session whose helper couldn't be restarted with the counter (a dismissed prompt) is a failure; without a
    // session it's the Administrator requirement. Either way a later start() asks again.
    return held ? UdpCollectorStartResult::Failed : UdpCollectorStartResult::NotElevated;
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
    // Folded under m_Mutex, then logged and published outside it: a log sink can block, and problem() is read on the
    // UI thread, from its own snapshot (#3560 review).
    std::string warning;
    std::vector<UdpProcessTraffic> traffic = fold(run, reply, helperGone, session, sessionBefore, warning);
    if (!warning.empty())
    {
        spdlog::warn("UDP traffic per process: {}; the counters stay TCP only", warning);
    }
    return traffic;
}

std::vector<UdpProcessTraffic> BrokeredUdpTrafficCollector::fold(std::uint64_t run,
                                                                 const Broker::UdpBytesReply* reply,
                                                                 bool helperGone,
                                                                 std::uint64_t session,
                                                                 std::uint64_t sessionBefore,
                                                                 std::string& warning)
{
    std::vector<UdpProcessTraffic> traffic;
    std::string problem;
    std::uint64_t version = 0;
    {
        const std::scoped_lock lock(m_Mutex);
        traffic = [&] -> std::vector<UdpProcessTraffic>
        {
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
                    m_Unsupported = reply->status == Broker::UdpBytesStatus::Failed;
                    problem =
                        reply->status == Broker::UdpBytesStatus::NotCounting ? std::string("The helper isn't counting UDP") : reply->detail;
                }
                if (problem != m_Problem)
                {
                    warning = problem; // logged once the lock is released: once per change, not per reading
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
            m_Unsupported = false;
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
        }();
        problem = m_Problem;
        version = ++m_ProblemVersion;
    }
    publishProblem(problem, version); // the UI's snapshot, outside m_Mutex
    return traffic;
}

bool BrokeredUdpTrafficCollector::unsupported() const
{
    return m_Unsupported.load();
}

std::string BrokeredUdpTrafficCollector::problem() const
{
    const std::scoped_lock lock(m_StatusMutex);
    return m_StatusProblem;
}

void BrokeredUdpTrafficCollector::publishProblem(const std::string& problem, std::uint64_t version) noexcept
{
    try
    {
        // Each change of m_Problem takes the next version under m_Mutex, and is published after it's released: a
        // version older than the snapshot's lost a race with a later change (start() against a sample), so it's dropped.
        const std::scoped_lock lock(m_StatusMutex);
        if (version > m_StatusVersion)
        {
            m_StatusVersion = version;
            m_StatusProblem = problem;
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): an allocation failure leaves the last reason showing
    {}
}

void BrokeredUdpTrafficCollector::setProblem(const std::string& problem)
{
    std::uint64_t version = 0;
    {
        const std::scoped_lock lock(m_Mutex);
        m_Problem = problem;
        version = ++m_ProblemVersion;
    }
    publishProblem(problem, version);
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
