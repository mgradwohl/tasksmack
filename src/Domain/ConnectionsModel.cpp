#include "ConnectionsModel.h"

#include "Domain/ProcessConnections.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

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
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
