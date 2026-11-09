#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Platform/ISystemConnections.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// One socket's transfer rates between two readings (#3079); unknown on the first reading of a socket, for one
/// without byte counters (UDP, the /proc/net fallback), or when a counter went backwards.
struct ConnectionRate
{
    bool known = false;
    double sentPerSec = 0.0;
    double receivedPerSec = 0.0;
};

/// One immutable generation of the machine's sockets (#3066).
struct ConnectionsPublication
{
    std::uint64_t version = 0; ///< 0 until the first sample.
    Platform::SystemConnectionsStatus status = Platform::SystemConnectionsStatus::Unsupported;
    std::string detail;                                  ///< For Failed, what failed.
    std::string ratesUnavailable;                        ///< Why TCP sockets have no rates (#3356), or empty
    std::vector<Platform::SystemConnection> connections; ///< By process name, then PID, protocol and local endpoint.
    std::vector<ConnectionRate> rates;                   ///< Parallel to connections (#3079)
};

/// A socket's byte counters at one reading, by Connections::socketKey().
struct ConnectionBytes
{
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
};

/// The rates of @p connections against @p previous, the counters read @p elapsedSeconds earlier (#3079). Pure.
[[nodiscard]] std::vector<ConnectionRate> connectionRates(const std::vector<Platform::SystemConnection>& connections,
                                                          const std::unordered_map<std::uint64_t, ConnectionBytes>& previous,
                                                          double elapsedSeconds);

/// Samples every socket off the UI thread (from the Connections tab's BackgroundSampler) and publishes
/// each reading as an immutable ConnectionsPublication, so the UI reads it without locking or copying.
class ConnectionsModel : public ISamplable
{
  public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;

    /// @param now Times each reading, for the rates (tests: a fake clock).
    explicit ConnectionsModel(std::unique_ptr<Platform::ISystemConnectionsReader> reader, NowFunction now = [] { return Clock::now(); });

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
    NowFunction m_Now;
    bool m_HasConnections = false;
    std::mutex m_SampleMutex;
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    // The last reading's byte counters and when it was taken, for the next one's rates (#3079); guarded by m_SampleMutex.
    std::unordered_map<std::uint64_t, ConnectionBytes> m_PreviousBytes;
    // Both change together, at the end of a sample that can no longer throw (#3400).
    Clock::time_point m_PreviousAt;
    PublicationSlot<ConnectionsPublication> m_Slot;
};

} // namespace Domain
