#pragma once

// Per-process UDP bytes from the privileged helper's eBPF counter (#3366): Linux's IUdpTrafficCollector, the
// counterpart of Windows' ETW collector. It asks the broker to start a helper that counts, but only within an
// Administrator session (maintainer, 2026-11-07), and reads the helper's totals once per socket-traffic reading.

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IUdpTrafficCollector.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

    /// Asks the broker for a counting helper (IPrivilegedBroker::countUdpBytes()). NotElevated without a live
    /// Administrator session, where nothing prompts; Failed when the session's helper couldn't be restarted with the
    /// counter. Either way it stays armed, as the request stands in the broker: once a session's helper counts,
    /// sample() delivers; and a later start() asks again. Started at once when it already runs with nothing in the way.
    [[nodiscard]] UdpCollectorStartResult start() override;
    void stop() override;
    /// Whether the last reading came from a helper that counts.
    [[nodiscard]] bool isDelivering() const override;
    /// Every process's cumulative UDP bytes, as the helper counts them, for Domain to compare reading by reading:
    ///  - the first counting reading of a run, or after a failed one, is baseline-only (readable, baselineOnly): Domain
    ///    takes it as the baseline, crediting nothing, as a helper that kept counting meanwhile has older bytes in it,
    ///    and the very next reading credits the growth from it;
    ///  - a busy helper's reading is the last one again, unreadable (readable false): Domain credits nothing for it and
    ///    re-baselines on the next readable one, whose growth spans it;
    ///  - a new helper's counters are another generation: new connections, counted from their first bytes.
    /// Empty when stopped, or when the helper isn't counting or has ended (the reason is in problem()).
    [[nodiscard]] std::vector<UdpProcessTraffic> sample() override;

    /// Why it isn't counting, as start() or the helper last said; empty while it counts or before start().
    [[nodiscard]] std::string problem() const override;
    /// Whether the helper last said it couldn't load or read its counter (UdpBytesStatus::Failed): this system can't
    /// count, until a helper does.
    [[nodiscard]] bool unsupported() const override;

  private:
    void setProblem(const std::string& problem);
    /// Copies @p problem into the snapshot problem() reads, under its own short lock, unless a later @p version is there.
    void publishProblem(const std::string& problem, std::uint64_t version) noexcept;
    /// sample()'s reading folded in under m_Mutex; a changed reason to log goes to @p warning, for sample() to log once
    /// the lock is released.
    [[nodiscard]] std::vector<UdpProcessTraffic> fold(std::uint64_t run,
                                                      const Broker::UdpBytesReply* reply,
                                                      bool helperGone,
                                                      std::uint64_t session,
                                                      std::uint64_t sessionBefore,
                                                      std::string& warning);

    std::shared_ptr<IPrivilegedBroker> m_Broker;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Delivering{false};
    std::atomic<bool> m_Unsupported{false};
    mutable std::mutex m_Mutex;            // guards the members below, and m_Running's changes
    std::uint64_t m_Run = 0;               // one more each start() and stop(): a read of another run publishes nothing
    std::uint32_t m_Generation = 0;        // the counters' generation (UdpProcessTraffic::generation)
    std::uint64_t m_Session = 0;           // the broker's sessionsStarted() when last read: a change is another helper
    bool m_Seeding = false;                // the next counting reading only seeds Domain's baseline (sent unreadable)
    std::vector<UdpProcessTraffic> m_Last; // the last totals, for a reading the helper was too busy to answer
    std::string m_Problem;
    std::uint64_t m_ProblemVersion = 0; // one more each time m_Problem is set, under m_Mutex
    mutable std::mutex m_StatusMutex;   // guards m_StatusProblem alone: held only to copy it, never across I/O or logging
    std::uint64_t m_StatusVersion = 0;  // the version m_StatusProblem came from
    std::string m_StatusProblem;        // what problem() returns: m_Problem, as each fold, start() or setProblem() left it
};

/// Linux's UDP collector: @p collector when the platform made one (Windows' ETW), else one through @p broker, or
/// null without either.
[[nodiscard]] std::shared_ptr<IUdpTrafficCollector> withPrivilegedBroker(std::shared_ptr<IUdpTrafficCollector> collector,
                                                                         std::shared_ptr<IPrivilegedBroker> broker);

} // namespace Platform
