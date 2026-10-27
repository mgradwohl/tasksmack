#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Platform/ISystemConnections.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Domain
{

/// One immutable generation of the machine's sockets (#3066).
struct ConnectionsPublication
{
    std::uint64_t version = 0; ///< 0 until the first sample.
    Platform::SystemConnectionsStatus status = Platform::SystemConnectionsStatus::Unsupported;
    std::string detail;                                  ///< For Failed, what failed.
    std::vector<Platform::SystemConnection> connections; ///< By process name, then PID, protocol and local endpoint.
};

/// Samples every socket off the UI thread (from the Connections tab's BackgroundSampler) and publishes
/// each reading as an immutable ConnectionsPublication, so the UI reads it without locking or copying.
class ConnectionsModel : public ISamplable
{
  public:
    explicit ConnectionsModel(std::unique_ptr<Platform::ISystemConnectionsReader> reader);

    /// Reads the sockets and publishes them. Thread-safe; samples are serialised.
    void sample() override;

    /// The newest generation (an empty version-0 one before the first sample).
    [[nodiscard]] std::shared_ptr<const ConnectionsPublication> publication() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

    /// Whether the reader can list the machine's sockets, fixed at construction.
    [[nodiscard]] bool hasConnections() const noexcept
    {
        return m_HasConnections;
    }

  private:
    std::unique_ptr<Platform::ISystemConnectionsReader> m_Reader; // used under m_SampleMutex only
    bool m_HasConnections = false;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    PublicationSlot<ConnectionsPublication> m_Slot;
};

} // namespace Domain
