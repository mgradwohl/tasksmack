#pragma once

#include <cstdint>
#include <vector>

namespace Platform
{

/// One process's UDP bytes since the collector started (#1492): cumulative counters that only grow
/// while the collector runs, so Domain::SocketTrafficAccumulator takes them as one long-lived
/// "connection" per process.
struct UdpProcessTraffic
{
    std::uint32_t pid = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
};

/// Why start() left the collector stopped, or that it runs.
enum class UdpCollectorStartResult : std::uint8_t
{
    Started,     ///< The trace session runs (UDP is counted once its first event arrives)
    NotElevated, ///< Needs an elevated (administrator) process; nothing was tried
    Denied,      ///< The OS refused to start the session (access denied)
    Failed,      ///< Any other failure (no session slots left, provider refused, ...)
};

/// Per-process UDP byte counters from an opt-in kernel trace (#1492; Windows: an ETW real-time
/// session on Microsoft-Windows-Kernel-Network). Owned by App, which creates it with
/// Platform::makeUdpTrafficCollector() and starts or stops it as the Settings toggle says; the process
/// probe shares it and folds its counters into readSocketTraffic(). start() and stop() may block (a
/// stop joins the consumer thread), so App calls them off the UI thread: from the sampler's start hook,
/// then one at a time on its switch's worker (App::UdpTraceSwitch, #3155). isDelivering() and sample()
/// may be called from any thread.
class IUdpTrafficCollector
{
  public:
    virtual ~IUdpTrafficCollector() = default;

    IUdpTrafficCollector() = default;
    IUdpTrafficCollector(const IUdpTrafficCollector&) = delete;
    IUdpTrafficCollector& operator=(const IUdpTrafficCollector&) = delete;
    IUdpTrafficCollector(IUdpTrafficCollector&&) = delete;
    IUdpTrafficCollector& operator=(IUdpTrafficCollector&&) = delete;

    /// Start collecting. Already running: returns Started without restarting.
    [[nodiscard]] virtual UdpCollectorStartResult start() = 0;

    /// Stop collecting and forget the counters. Safe to call when stopped.
    virtual void stop() = 0;

    /// Whether the collector runs and has delivered at least one UDP event since it started: only
    /// then do the process counters include UDP (ProcessCapabilities::hasUdpNetworkCounters).
    [[nodiscard]] virtual bool isDelivering() const = 0;

    /// Every process's cumulative UDP bytes since start(), read once per socket-traffic reading. A
    /// process whose counters have not grown for a while is left out from then on, and starts again
    /// from 0 if it sends again (Domain takes it as new then). Empty when stopped.
    [[nodiscard]] virtual std::vector<UdpProcessTraffic> sample() = 0;
};

} // namespace Platform
