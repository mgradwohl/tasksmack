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

namespace Domain
{

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
///  - A connection whose counters couldn't be read this time (SocketTrafficSample::readable false,
///    #1256) is still open: it keeps its previous baseline unchanged, credits nothing, and the next
///    readable sample credits all the growth since the last readable one. One first seen unreadable
///    is recorded as seen with no baseline, so its first readable sample only sets the baseline:
///    crediting it as new would land the lifetime bytes of a connection that may have been open for
///    hours in one interval. The cost is the bytes a connection that really did open while unreadable
///    moved before its first readable sample, the same as for one attributed late. An unreadable
///    sample's owner is not used: bytes are credited to the owner reported with them.
/// Bytes a connection moves between the last reading and its close, or before it is attributed, are
/// not counted. Feed only complete readings: a connection missing from a partial one would come back
/// as "new" and credit its lifetime bytes.
///
/// Probes report the raw per-connection counters (Platform::IProcessProbe::readSocketTraffic());
/// ProcessModel owns one of these and applies each refresh's reading with apply(). Not thread-safe:
/// ProcessModel calls it under its sampling lock.
class SocketTrafficAccumulator
{
  public:
    /// Fold `reading` in if it was taken strictly after the last one folded, then write every
    /// process's cumulative totals into its netReceivedBytes/netSentBytes and the time of the reading
    /// they come from into its netSampleTimeNs -- processes with connections or not, so ProcessModel
    /// takes rates over the time between real readings rather than between refreshes (#1063 review).
    ///  - A probe caches its query, so the same reading can come back for several refreshes; it is
    ///    folded once. An older one is never folded: it would rewind the connection baselines and
    ///    count the traffic in between twice.
    ///  - A failed reading (sampleTimeNs 0) is not folded either -- a connection missing from it would
    ///    look closed and then, back in the next reading, new. Like a repeated one, it republishes the
    ///    last reading's totals and time, so the model holds the last rate instead of measuring a 0
    ///    and then two intervals' bytes over one.
    ///  - Only a refresh that folds a new reading prunes the totals of exited processes; the others
    ///    only read them (publishTotals()).
    /// Until a reading has been folded `processes` is left untouched, so a probe that never returns
    /// readings keeps reporting its own counters.
    void apply(const Platform::SocketTrafficReading& reading, std::vector<Platform::ProcessCounters>& processes)
    {
        const bool folded = reading.sampleTimeNs != 0 && reading.sampleTimeNs > m_LastReadingTimeNs;
        if (folded)
        {
            addReading(reading.sockets);
            m_LastReadingTimeNs = reading.sampleTimeNs;
        }
        if (m_LastReadingTimeNs == 0)
        {
            return;
        }
        for (auto& proc : processes)
        {
            proc.netSampleTimeNs = m_LastReadingTimeNs;
        }
        if (folded)
        {
            publish(processes);
        }
        else
        {
            publishTotals(processes);
        }
    }

    /// Fold one complete reading of every connection into the per-process totals. The credited bytes
    /// are held until the next publish().
    void addReading(std::span<const Platform::SocketTrafficSample> sockets)
    {
        std::unordered_map<std::uint64_t, SocketState> next;
        next.reserve(sockets.size());
        for (const auto& sample : sockets)
        {
            if (sample.key == 0)
            {
                continue;
            }
            const auto previous = m_Sockets.find(sample.key);
            if (!sample.readable)
            {
                // Still open, counters unknown this time: keep the baseline (or the "seen, no
                // baseline" mark) as it was, credit nothing (#1256).
                next.insert_or_assign(sample.key, (previous != m_Sockets.end()) ? previous->second : SocketState{.hasBaseline = false});
                continue;
            }
            Totals credit;
            if (previous != m_Sockets.end())
            {
                if (!previous->second.hasBaseline)
                {
                    // First readable sample of a connection first seen unreadable: its bytes may
                    // predate this interval, so this only sets the baseline (#1256).
                    next.insert_or_assign(sample.key, SocketState{.bytesReceived = sample.bytesReceived, .bytesSent = sample.bytesSent});
                    continue;
                }
                // A counter that went backwards in either direction means a different connection is
                // reusing the key: nothing is credited for it this interval in either direction (its
                // other counter isn't comparable with the old connection's either); both are the new
                // baseline.
                const bool reused = sample.bytesReceived < previous->second.bytesReceived || sample.bytesSent < previous->second.bytesSent;
                if (!reused)
                {
                    credit.received = sample.bytesReceived - previous->second.bytesReceived;
                    credit.sent = sample.bytesSent - previous->second.bytesSent;
                }
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

    /// Write every process's current cumulative totals into its netReceivedBytes/netSentBytes without
    /// changing any state: no pending bytes are credited and no process is forgotten. For a caller
    /// that didn't fold a new reading (a cached, failed, or older one that was skipped): processes are
    /// forgotten only in step with the reading that credits them, so a process list that doesn't
    /// match the newest reading can never delete a process's totals and break monotonicity.
    void publishTotals(std::vector<Platform::ProcessCounters>& processes) const
    {
        for (auto& proc : processes)
        {
            const ProcessKey key{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
            const auto existing = m_Totals.find(key);
            proc.netReceivedBytes = (existing != m_Totals.end()) ? existing->second.received : 0;
            proc.netSentBytes = (existing != m_Totals.end()) ? existing->second.sent : 0;
        }
    }

    /// Credit the bytes held since the last publish() to the processes they belong to, write every
    /// process's cumulative totals into its netReceivedBytes/netSentBytes, and forget the totals of
    /// processes that are gone. A process is identified by PID and start time, so a reused PID starts
    /// again from 0; bytes held for a PID that isn't in `processes` are dropped.
    void publish(std::vector<Platform::ProcessCounters>& processes)
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
        m_LastReadingTimeNs = 0;
    }

  private:
    struct SocketState
    {
        std::uint64_t bytesReceived = 0;
        std::uint64_t bytesSent = 0;
        bool hasBaseline = true; // False: seen only unreadable so far; the byte fields mean nothing (#1256)
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

    [[nodiscard]] static constexpr std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept
    {
        constexpr auto MAX_BYTES = std::numeric_limits<std::uint64_t>::max();
        return a > MAX_BYTES - b ? MAX_BYTES : a + b;
    }

    std::unordered_map<std::uint64_t, SocketState> m_Sockets;        // last reading, by connection key
    std::unordered_map<std::int32_t, Totals> m_PendingByPid;         // credited since the last publish()
    std::unordered_map<ProcessKey, Totals, ProcessKeyHash> m_Totals; // cumulative bytes per live process
    bool m_HasReading = false;
    std::uint64_t m_LastReadingTimeNs = 0; // sampleTimeNs of the last reading apply() folded; 0 = none
};

} // namespace Domain
