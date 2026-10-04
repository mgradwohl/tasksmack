#pragma once

#include "Platform/ProcessTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform
{

/// One connection's cumulative byte counters as the OS reports them, and the process it belongs to.
struct SocketTrafficSample
{
    std::uint64_t key = 0; // Stable identity of the connection for its lifetime: the socket inode on Linux
    std::int32_t pid = 0;  // Owning process; 0 = not attributed (yet)
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
};

/// Turns readings of per-connection byte counters into a monotonic byte counter per process (#1099).
///
/// A process's network counter used to be the sum of the bytes on the connections it had open at the
/// moment of the read. That sum isn't monotonic: when a connection closed its lifetime bytes left the
/// sum, the delta went negative and the whole process read 0 for the interval, however much its other
/// connections moved; and a connection attributed late (the inode-to-PID map is rebuilt every few
/// seconds) delivered its lifetime bytes in one interval, a one-sample spike.
///
/// Instead, each reading is compared with the previous one connection by connection:
///  - A connection present in both credits its own growth to its current owner. A connection that
///    closes stops crediting but takes nothing back, and one whose owner changes (a socket inherited
///    across fork() and attributed to the other process) moves only the bytes of this interval.
///  - A connection's counters are tracked from its first reading even while it has no owner, so one
///    attributed late credits only the growth since the previous reading, never its earlier bytes.
///  - A connection that is new since the previous reading credits all its bytes to its owner: it
///    opened within this interval, so all of them were sent within it. On the first reading every
///    connection is new and only sets the baseline.
/// Bytes a connection moves between the last reading and its close, or before it is attributed, are
/// not counted. Feed only complete readings: a connection missing from a partial one would come back
/// as "new" and credit its lifetime bytes.
///
/// Platform-neutral so every process probe can use it; not thread-safe (callers serialize access).
class SocketTrafficAccumulator
{
  public:
    /// Fold one complete reading of every connection into the per-process totals. The credited bytes
    /// are held until the next publish().
    void addReading(std::span<const SocketTrafficSample> sockets)
    {
        std::unordered_map<std::uint64_t, SocketState> next;
        next.reserve(sockets.size());
        for (const auto& sample : sockets)
        {
            if (sample.key == 0)
            {
                continue;
            }
            Totals credit;
            if (const auto previous = m_Sockets.find(sample.key); previous != m_Sockets.end())
            {
                // A counter that went backwards belongs to a different connection reusing the key:
                // nothing is credited for it this interval; it is the new baseline.
                credit.received = growth(sample.bytesReceived, previous->second.bytesReceived);
                credit.sent = growth(sample.bytesSent, previous->second.bytesSent);
            }
            else if (m_HasReading)
            {
                credit.received = sample.bytesReceived;
                credit.sent = sample.bytesSent;
            }
            if (sample.pid > 0 && (credit.received != 0 || credit.sent != 0))
            {
                m_PendingByPid[sample.pid].add(credit);
            }
            next.insert_or_assign(sample.key, SocketState{.bytesReceived = sample.bytesReceived, .bytesSent = sample.bytesSent});
        }
        m_Sockets = std::move(next);
        m_HasReading = true;
    }

    /// Credit the bytes held since the last publish() to the processes they belong to, write every
    /// process's cumulative totals into its netReceivedBytes/netSentBytes, and forget the totals of
    /// processes that are gone. A process is identified by PID and start time, so a reused PID starts
    /// again from 0; bytes held for a PID that isn't in `processes` are dropped.
    void publish(std::vector<ProcessCounters>& processes)
    {
        std::unordered_map<ProcessKey, Totals, ProcessKeyHash> live;
        live.reserve(processes.size());
        for (auto& proc : processes)
        {
            const ProcessKey key{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
            Totals totals;
            if (const auto existing = m_Totals.find(key); existing != m_Totals.end())
            {
                totals = existing->second;
            }
            if (const auto pending = m_PendingByPid.find(proc.pid); pending != m_PendingByPid.end())
            {
                totals.add(pending->second);
            }
            proc.netReceivedBytes = totals.received;
            proc.netSentBytes = totals.sent;
            if (totals.received != 0 || totals.sent != 0)
            {
                live.insert_or_assign(key, totals);
            }
        }
        m_Totals = std::move(live);
        m_PendingByPid.clear();
    }

    /// Forget every connection and total, as if no reading had been taken.
    void reset()
    {
        m_Sockets.clear();
        m_PendingByPid.clear();
        m_Totals.clear();
        m_HasReading = false;
    }

  private:
    struct SocketState
    {
        std::uint64_t bytesReceived = 0;
        std::uint64_t bytesSent = 0;
    };

    struct Totals
    {
        std::uint64_t received = 0;
        std::uint64_t sent = 0;

        void add(const Totals& other) noexcept
        {
            received = saturatingAdd(received, other.received);
            sent = saturatingAdd(sent, other.sent);
        }
    };

    struct ProcessKey
    {
        std::int32_t pid = 0;
        std::uint64_t startTimeTicks = 0;
        friend bool operator==(const ProcessKey&, const ProcessKey&) = default;
    };

    struct ProcessKeyHash
    {
        [[nodiscard]] std::size_t operator()(const ProcessKey& key) const noexcept
        {
            const std::size_t pidHash = std::hash<std::int32_t>{}(key.pid);
            const std::size_t startHash = std::hash<std::uint64_t>{}(key.startTimeTicks);
            return pidHash ^ (startHash + 0x9e3779b97f4a7c15ULL + (pidHash << 6U) + (pidHash >> 2U));
        }
    };

    [[nodiscard]] static constexpr std::uint64_t growth(std::uint64_t now, std::uint64_t before) noexcept
    {
        return now >= before ? now - before : 0;
    }

    [[nodiscard]] static constexpr std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept
    {
        constexpr auto MAX_BYTES = std::numeric_limits<std::uint64_t>::max();
        return a > MAX_BYTES - b ? MAX_BYTES : a + b;
    }

    std::unordered_map<std::uint64_t, SocketState> m_Sockets;        // last reading, by connection key
    std::unordered_map<std::int32_t, Totals> m_PendingByPid;         // credited since the last publish()
    std::unordered_map<ProcessKey, Totals, ProcessKeyHash> m_Totals; // cumulative bytes per live process
    bool m_HasReading = false;
};

} // namespace Platform
