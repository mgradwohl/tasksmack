#include "LinuxUdpTrafficCollector.h"

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/Linux/UdpByteCounter.h"
#include "Platform/UdpByteCounts.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

[[nodiscard]] std::expected<LinuxUdpTrafficCollector::Read, std::string> loadCounter()
{
    auto loaded = UdpByteCounter::load();
    if (!loaded.has_value())
    {
        return std::unexpected(std::move(loaded.error()));
    }
    // Shared, as a std::function must be copyable and the counter is move-only.
    const auto counter = std::make_shared<UdpByteCounter>(std::move(*loaded));
    return LinuxUdpTrafficCollector::Read{[counter] { return counter->read(); }};
}

} // namespace

LinuxUdpTrafficCollector::LinuxUdpTrafficCollector() : LinuxUdpTrafficCollector(Load{loadCounter})
{}

LinuxUdpTrafficCollector::LinuxUdpTrafficCollector(Load load) : m_Load(std::move(load))
{}

UdpCollectorStartResult LinuxUdpTrafficCollector::start()
{
    {
        const std::scoped_lock lock(m_Mutex);
        if (m_Read != nullptr)
        {
            return UdpCollectorStartResult::Started;
        }
    }
    // Loading verifies and attaches the program, which takes a while: never under the lock problem() takes.
    auto loaded = m_Load();
    if (!loaded.has_value())
    {
        spdlog::warn("UDP traffic per process: the eBPF counter didn't load: {}; the counters stay TCP only", loaded.error());
        const std::scoped_lock lock(m_Mutex);
        m_Problem = std::move(loaded.error());
        publishProblem();
        m_Unsupported = true;
        return UdpCollectorStartResult::Failed;
    }
    {
        const std::scoped_lock lock(m_Mutex);
        ++m_Run;
        ++m_Generation;
        m_Seeding = false; // a new counter counts from zero: its first reading is all new bytes
        m_Read = std::make_shared<Read>(std::move(*loaded));
        m_Problem.clear();
        publishProblem();
        m_Unsupported = false;
    }
    spdlog::info("UDP traffic per process: counting in TaskSmack's own process (running as root)");
    return UdpCollectorStartResult::Started;
}

void LinuxUdpTrafficCollector::stop()
{
    std::shared_ptr<Read> unloaded;
    {
        const std::scoped_lock lock(m_Mutex);
        ++m_Run;
        unloaded = std::exchange(m_Read, nullptr);
        m_Delivering = false;
    }
    // The counter detaches when the last read holding it ends: here, or when a sample() in flight returns.
}

bool LinuxUdpTrafficCollector::isDelivering() const
{
    return m_Delivering.load();
}

std::vector<UdpProcessTraffic> LinuxUdpTrafficCollector::sample()
{
    std::shared_ptr<Read> read;
    std::uint64_t run = 0;
    {
        const std::scoped_lock lock(m_Mutex);
        read = m_Read;
        run = m_Run;
    }
    if (read == nullptr)
    {
        return {};
    }
    auto totals = (*read)();
    std::vector<UdpProcessTraffic> traffic;
    std::string warning;
    {
        const std::scoped_lock lock(m_Mutex);
        if (run != m_Run)
        {
            return {}; // stopped, or stopped and started again, while this read was out
        }
        if (!totals.has_value())
        {
            if (m_Problem != totals.error())
            {
                warning = totals.error(); // logged once the lock is released, once per change
            }
            m_Problem = std::move(totals.error());
            publishProblem();
            m_Delivering = false;
            m_Seeding = true;
        }
        else
        {
            const bool seeding = m_Seeding;
            m_Seeding = false;
            traffic.reserve(totals->size());
            for (const UdpByteCounts& process : *totals)
            {
                traffic.push_back({
                    .pid = process.tgid,
                    .bytesSent = process.txBytes,
                    .bytesReceived = process.rxBytes,
                    .generation = m_Generation,
                    .readable = true,
                    .baselineOnly = seeding,
                });
            }
            if (!m_Problem.empty())
            {
                m_Problem.clear();
                publishProblem();
            }
            m_Delivering = true;
        }
    }
    if (!warning.empty())
    {
        spdlog::warn("UDP traffic per process: {}; the counters stay TCP only", warning);
    }
    return traffic;
}

std::string LinuxUdpTrafficCollector::problem() const
{
    const std::scoped_lock lock(m_StatusMutex);
    return m_StatusProblem;
}

void LinuxUdpTrafficCollector::publishProblem()
{
    const std::scoped_lock lock(m_StatusMutex);
    m_StatusProblem = m_Problem;
}

bool LinuxUdpTrafficCollector::unsupported() const
{
    return m_Unsupported.load();
}

} // namespace Platform
