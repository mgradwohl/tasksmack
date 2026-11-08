#pragma once

// Per-process UDP bytes from the privileged helper's eBPF counter (#3366): Linux's IUdpTrafficCollector, the
// counterpart of Windows' ETW collector. It asks the broker to start a helper that counts, but only within an
// Administrator session (maintainer, 2026-11-07), and reads the helper's totals once per socket-traffic reading.

#include "Platform/IPrivilegedBroker.h"
#include "Platform/IUdpTrafficCollector.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Platform
{

class BrokeredUdpTrafficCollector final : public IUdpTrafficCollector
{
  public:
    /// How long sample() waits for the helper's answer: it runs on the process sampler, which mustn't stall.
    static constexpr std::chrono::milliseconds SAMPLE_TIMEOUT{500};

    explicit BrokeredUdpTrafficCollector(std::shared_ptr<IPrivilegedBroker> broker);

    /// Asks the broker for a counting helper (IPrivilegedBroker::countUdpBytes()). NotElevated without an
    /// Administrator session, where nothing prompts; Failed when no counting helper could be started.
    [[nodiscard]] UdpCollectorStartResult start() override;
    void stop() override;
    /// Whether the last reading came from a helper that counts.
    [[nodiscard]] bool isDelivering() const override;
    /// Every process's totals from the helper, or the last ones when it was busy. Empty when stopped, or when the
    /// helper isn't counting (its reason is in problem()).
    [[nodiscard]] std::vector<UdpProcessTraffic> sample() override;

    /// Why it isn't counting, as start() or the helper last said; empty while it counts or before start().
    [[nodiscard]] std::string problem() const;

  private:
    void setProblem(std::string problem);

    std::shared_ptr<IPrivilegedBroker> m_Broker;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Delivering{false};
    mutable std::mutex m_Mutex;            // guards the two below
    std::vector<UdpProcessTraffic> m_Last; // the last totals, for a reading the helper was too busy to answer
    std::string m_Problem;
};

/// Linux's UDP collector: @p collector when the platform made one (Windows' ETW), else one through @p broker, or
/// null without either.
[[nodiscard]] std::shared_ptr<IUdpTrafficCollector> withPrivilegedBroker(std::shared_ptr<IUdpTrafficCollector> collector,
                                                                         std::shared_ptr<IPrivilegedBroker> broker);

} // namespace Platform
