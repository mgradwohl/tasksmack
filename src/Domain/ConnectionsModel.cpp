#include "ConnectionsModel.h"

#include "Domain/ProcessConnections.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Domain
{

std::vector<ConnectionRate> connectionRates(const std::vector<Platform::SystemConnection>& connections,
                                            const std::unordered_map<std::uint64_t, ConnectionBytes>& previous,
                                            double elapsedSeconds)
{
    std::vector<ConnectionRate> rates(connections.size());
    if (elapsedSeconds <= 0.0)
    {
        return rates;
    }
    for (std::size_t i = 0; i < connections.size(); ++i)
    {
        const Platform::SystemConnection& connection = connections[i];
        if (!connection.hasByteCounters)
        {
            continue;
        }
        const auto before = previous.find(Connections::socketKey(connection.connection));
        // A new socket, or one whose counters went backwards (closed and reopened on the same endpoints): no rate yet.
        if (before == previous.end() || connection.bytesSent < before->second.sent || connection.bytesReceived < before->second.received)
        {
            continue;
        }
        rates[i] =
            ConnectionRate{.known = true,
                           .sentPerSec = static_cast<double>(connection.bytesSent - before->second.sent) / elapsedSeconds,
                           .receivedPerSec = static_cast<double>(connection.bytesReceived - before->second.received) / elapsedSeconds};
    }
    return rates;
}

ConnectionsModel::ConnectionsModel(std::unique_ptr<Platform::ISystemConnectionsReader> reader) : m_Reader(std::move(reader))
{
    if (!m_Reader)
    {
        throw std::invalid_argument("ConnectionsModel requires a reader");
    }
    m_HasConnections = m_Reader->hasConnections();
}

void ConnectionsModel::sample()
{
    const std::scoped_lock lock(m_SampleMutex);

    Platform::SystemConnectionsRead read = m_Reader->read();
    auto next = std::make_shared<ConnectionsPublication>();
    next->status = read.status;
    next->detail = std::move(read.detail);
    next->connections = std::move(read.connections);
    // The table's default order, once per sample here rather than per frame in the UI: by holder (those
    // without one last), then the socket.
    std::ranges::stable_sort(next->connections,
                             [](const Platform::SystemConnection& a, const Platform::SystemConnection& b)
                             {
                                 if ((a.pid == 0) != (b.pid == 0))
                                 {
                                     return b.pid == 0;
                                 }
                                 if (const auto byName = Platform::compareIgnoreCase(a.processName, b.processName); byName != 0)
                                 {
                                     return byName < 0;
                                 }
                                 if (a.pid != b.pid)
                                 {
                                     return a.pid < b.pid;
                                 }
                                 if (a.connection.protocol != b.connection.protocol)
                                 {
                                     return a.connection.protocol < b.connection.protocol;
                                 }
                                 return Connections::compareEndpoints(
                                            a.connection.family, a.connection.local, b.connection.family, b.connection.local) < 0;
                             });
    // Rates against the last reading (#3079), then this reading's counters kept for the next.
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = m_PreviousBytes.empty() ? 0.0 : std::chrono::duration<double>(now - m_PreviousAt).count();
    next->rates = connectionRates(next->connections, m_PreviousBytes, elapsed);
    m_PreviousBytes.clear();
    for (const Platform::SystemConnection& connection : next->connections)
    {
        if (connection.hasByteCounters)
        {
            m_PreviousBytes.insert_or_assign(Connections::socketKey(connection.connection),
                                             ConnectionBytes{.sent = connection.bytesSent, .received = connection.bytesReceived});
        }
    }
    m_PreviousAt = now;
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
