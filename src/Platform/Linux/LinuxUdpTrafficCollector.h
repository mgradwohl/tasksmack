#pragma once

// Per-process UDP bytes counted in TaskSmack's own process (#3580): Linux's IUdpTrafficCollector when TaskSmack runs
// as root. It loads the eBPF counter (UdpByteCounter, #3366) on start() and reads its map once
// per socket-traffic reading.

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/UdpByteCounts.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Platform
{

class LinuxUdpTrafficCollector final : public IUdpTrafficCollector
{
  public:
    /// One loaded counter's read: every process's totals since it loaded, or why the kernel refused reading them.
    using Read = std::function<std::expected<std::vector<UdpByteCounts>, std::string>()>;
    /// Loads a counter and returns its read, or why it couldn't load. The read owns the counter, which stays
    /// attached for as long as the read lives.
    using Load = std::function<std::expected<Read, std::string>()>;

    /// The real counter: UdpByteCounter::load(). Needs root.
    LinuxUdpTrafficCollector();
    /// @p load instead, for the tests.
    explicit LinuxUdpTrafficCollector(Load load);

    /// Loads the counter. Failed, with the reason in problem(), when it can't load; unsupported() is then true until
    /// a later start() loads it. Already running: Started without loading again.
    [[nodiscard]] UdpCollectorStartResult start() override;
    /// Unloads the counter and forgets its totals.
    void stop() override;
    /// Whether the last reading was read.
    [[nodiscard]] bool isDelivering() const override;
    /// Every process's totals since start(), for Domain to compare reading by reading:
    ///  - each start() loads a new counter from zero, so its totals are another generation, all of them new bytes;
    ///  - a reading the kernel refused is empty (Domain takes the connections as closed), and the next one read is
    ///    baseline-only, as its totals include the bytes of the interval that wasn't read.
    /// Empty when stopped.
    [[nodiscard]] std::vector<UdpProcessTraffic> sample() override;
    /// Why it isn't counting: the load's or the last read's error; empty while it counts or before start().
    [[nodiscard]] std::string problem() const override;
    /// Whether the counter couldn't load on this system (the last start() failed).
    [[nodiscard]] bool unsupported() const override;

  private:
    Load m_Load;
    std::atomic<bool> m_Delivering{false};
    std::atomic<bool> m_Unsupported{false};
    std::mutex m_Mutex;           // guards the members below; never held over a load or read, nor taken by problem()
    std::shared_ptr<Read> m_Read; // the loaded counter's read; null when stopped
    std::uint64_t m_Run = 0;      // one more each start() and stop(): a read of another run publishes nothing
    std::uint32_t m_Generation = 0;
    bool m_Seeding = false; // the next reading read only sets Domain's baseline
    std::string m_Problem;
    /// Guards m_StatusProblem alone, which problem() copies on the UI thread every frame: held only to copy it, so
    /// the UI never waits behind a sample's conversion under m_Mutex (#3589 review). Taken after m_Mutex, never before.
    mutable std::mutex m_StatusMutex;
    std::string m_StatusProblem;

    /// Copies m_Problem into m_StatusProblem. Called with m_Mutex held.
    void publishProblem();
};

} // namespace Platform
