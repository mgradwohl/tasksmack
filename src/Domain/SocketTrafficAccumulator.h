#pragma once

#include "Domain/SamplingConfig.h"
#include "Platform/ProcessTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
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
///  - A connection's counters are tracked from its first reading even while it has no owner. The
///    growth it shows while unowned is held for it and credited to the owner it is attributed to
///    later (#1259): the inode-to-PID map is rebuilt only every few seconds, and a new connection's
///    first bytes would otherwise never be counted. The bytes it already had when first seen unowned
///    are not held (they may predate the interval; the Linux probe rebuilds its map early so that a
///    new connection is normally attributed when first seen), and it holds growth only for
///    Sampling::UNATTRIBUTED_SOCKET_HOLD_MS after its first unowned sighting: an attributable
///    connection gets its owner within that time, and one that doesn't belongs to a process we can't
///    read and must not land hours of traffic in one interval if it is ever attributed.
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
///  - Credit goes to a process identified by PID and start time (ProcessKey) whenever both start times
///    are known (#1336); with either unknown it falls back to the PID, as described below. A connection's owner is reported with its start
///    time (SocketTrafficSample:: ownerStartTimeTicks); a listed process with the same PID but another start time is a different process --
///    the owner exited and its PID was reused, or the owner reused the PID of a process that exited -- so it gets none of the connection's
///    bytes, which are held as for an owner the refresh doesn't list yet. An owner start time of 0 (unknown: Windows, whose TCP tables
///    report only the owning PID) matches the listed process with that PID, whatever its start time.
/// Bytes a connection moves between the last reading and its close are not counted, nor are any of a
/// connection that closes before it gets an owner. Feed only complete readings: a connection missing
/// from a partial one would come back as "new" and credit its lifetime bytes.
///
/// Probes report the raw per-connection counters (Platform::IProcessProbe::readSocketTraffic());
/// ProcessModel owns one of these and applies each refresh's reading with apply(). Not thread-safe:
/// ProcessModel calls it under its sampling lock.
class SocketTrafficAccumulator
{
  public:
    /// The processes one refresh lists, by PID: each one's start time (ProcessCounters::startTimeTicks).
    using ListedProcesses = std::unordered_map<std::int32_t, std::uint64_t>;

    /// Fold `reading` in if it was taken strictly after the last one folded, then write every
    /// process's cumulative totals into its netReceivedBytes/netSentBytes and the time of the reading
    /// they come from into its netSampleTimeNs -- processes with connections or not, so ProcessModel
    /// takes rates over the time between real readings rather than between refreshes (#1063 review).
    ///  - A probe caches its query, so the same reading can come back for several refreshes; it is
    ///    folded once. An older one is never folded: it would rewind the connection baselines and
    ///    count the traffic in between twice.
    ///  - A repeat of the last reading can still bring new ownership: the Linux probe may rebuild its
    ///    inode-to-PID map early on a cached socket query (#1327 review). Its counters are ignored --
    ///    they were folded already -- but a connection that was unowned and now has an owner hands
    ///    that owner its held growth (reviseOwnership()). The bytes are credited with the next reading
    ///    folded, so they land in a measured interval rather than in a refresh that holds the rate, and
    ///    are kept even if the connection closes before then. Like all credit they are bound to the
    ///    owner's stable identity (PID and start time): if that process exits and its PID is reused
    ///    before then, the replacement gets none of them (#1327 review, #1336).
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
        const bool repeated = reading.sampleTimeNs != 0 && reading.sampleTimeNs == m_LastReadingTimeNs;
        if (folded || repeated)
        {
            // The processes this refresh lists: a connection attributed to a process that isn't listed
            // (it started between the process enumeration and the socket read, or it is another
            // process with a listed PID, #1336) keeps holding rather than having its bytes queued for
            // a process publish() would find nothing for (#1327 review).
            ListedProcesses listed;
            listed.reserve(processes.size());
            for (const auto& proc : processes)
            {
                listed.insert_or_assign(proc.pid, proc.startTimeTicks);
            }
            if (folded)
            {
                addReading(reading.sockets, reading.sampleTimeNs, &listed);
                m_LastReadingTimeNs = reading.sampleTimeNs;
            }
            else
            {
                reviseOwnership(reading.sockets, reading.sampleTimeNs, listed);
            }
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
    /// are held until the next publish(). `sampleTimeNs` is the reading's time (steady_clock ns): it
    /// bounds how long an unowned connection holds its growth (Sampling::UNATTRIBUTED_SOCKET_HOLD_MS);
    /// 0 (unknown) never ends a hold.
    /// `listedOwners`, when given, is the processes the same refresh lists: an attribution to an owner
    /// not among them (see listedOwner()) is treated as still unowned for this reading, so its held
    /// bytes survive to a reading whose process list includes the owner. Without it, bytes are
    /// queued for the owner as reported (PID and start time) and publish() credits them to it.
    void addReading(std::span<const Platform::SocketTrafficSample> sockets,
                    std::uint64_t sampleTimeNs = 0,
                    const ListedProcesses* listedOwners = nullptr)
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
                // baseline" mark) and any held growth as they were, credit nothing (#1256).
                next.insert_or_assign(sample.key, (previous != m_Sockets.end()) ? previous->second : SocketState{.hasBaseline = false});
                continue;
            }
            SocketState state{.bytesReceived = sample.bytesReceived, .bytesSent = sample.bytesSent};
            Totals credit;
            // Whether `credit` is growth since an earlier sighting of this same connection, which an
            // unowned connection may hold for its future owner; a new connection's first bytes and a
            // reused key's are not (they may predate the interval, or belong to another connection).
            bool creditIsGrowth = false;
            if (previous != m_Sockets.end())
            {
                // First readable sample of a connection first seen unreadable: its bytes may predate
                // this interval, so it only sets the baseline (#1256). Otherwise a counter that went
                // backwards in either direction means a different connection is reusing the key:
                // nothing is credited for it this interval in either direction (its other counter
                // isn't comparable with the old connection's either); both are the new baseline.
                const SocketState& prev = previous->second;
                const bool reused = sample.bytesReceived < prev.bytesReceived || sample.bytesSent < prev.bytesSent;
                if (prev.hasBaseline && !reused)
                {
                    credit.received = sample.bytesReceived - prev.bytesReceived;
                    credit.sent = sample.bytesSent - prev.bytesSent;
                    creditIsGrowth = true;
                    state = prev; // keeps an unowned run's held growth
                    state.bytesReceived = sample.bytesReceived;
                    state.bytesSent = sample.bytesSent;
                }
            }
            else if (m_HasReading)
            {
                // New since the previous reading: it opened within this interval.
                credit.received = sample.bytesReceived;
                credit.sent = sample.bytesSent;
            }

            const std::optional<ProcessKey> owner = listedOwner(sample, listedOwners);
            if (owner.has_value())
            {
                // Attributed: its owner gets this interval's bytes and whatever it held while unowned
                // (#1259), which ends its unowned run. The hold is judged at this reading's time: a
                // run whose deadline passed since its last unowned reading (a long suspend, a stalled
                // probe) has outlasted the hold, so its held bytes are dropped -- and so is this
                // interval's growth, which straddles the deadline and can't be split, rather than
                // landing as one interval's traffic.
                if (holdOutlasted(state, sampleTimeNs))
                {
                    credit = {};
                }
                else
                {
                    credit.add(state.held);
                }
                if (credit.received != 0 || credit.sent != 0)
                {
                    m_PendingByProcess[*owner].add(credit);
                }
                state = SocketState{.bytesReceived = sample.bytesReceived, .bytesSent = sample.bytesSent};
            }
            else if (sample.pid > 0)
            {
                // Attributed to a process this refresh doesn't list (yet), or to another process with a
                // listed PID (#1336): hold this interval's bytes too (a new connection's included), so
                // they reach the owner once a refresh lists it, and never the other process.
                holdUnownedGrowth(state, credit, sampleTimeNs);
            }
            else
            {
                holdUnownedGrowth(state, creditIsGrowth ? credit : Totals{}, sampleTimeNs);
            }
            next.insert_or_assign(sample.key, state);
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
    /// again from 0, and bytes queued for a process that isn't in `processes` with the same PID and
    /// start time are dropped. Bytes queued with an unknown start time (0) go to the process with
    /// that PID.
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
            if (const auto pending = m_PendingByProcess.find(key); pending != m_PendingByProcess.end())
            {
                totals.add(pending->second);
            }
            if (key.startTimeTicks != 0)
            {
                // Queued for this PID with no known start time: matched by PID alone.
                if (const auto pending = m_PendingByProcess.find(ProcessKey{.pid = proc.pid}); pending != m_PendingByProcess.end())
                {
                    totals.add(pending->second);
                }
            }
            proc.netReceivedBytes = totals.received;
            proc.netSentBytes = totals.sent;
            if (totals.received != 0 || totals.sent != 0)
            {
                live.insert_or_assign(key, totals);
            }
        }
        m_Totals = std::move(live);
        m_PendingByProcess.clear();
    }

    /// Forget every connection and total, as if no reading had been taken.
    void reset()
    {
        m_Sockets.clear();
        m_PendingByProcess.clear();
        m_Totals.clear();
        m_HasReading = false;
        m_LastReadingTimeNs = 0;
    }

  private:
    /// Apply a repeat of the last folded reading (same time, so the same counters) for the ownership
    /// it may add: a connection that was in an unowned run and now has an owner moves its held growth
    /// to that owner, credited by the next publish(), and its run ends -- so the held bytes are moved
    /// exactly once. The next publish() comes with a later refresh's process list, so the bytes are
    /// bound to the owner's PID and start time (listedOwner()), not to its PID alone: a PID reused in
    /// between gets none of them. An owner that isn't in `listed` can't be identified, so its
    /// connection is left as it was, for the next fresh reading. Counters are not
    /// compared and nothing else changes: an unreadable sample's owner is not used, an owned
    /// connection keeps its owner until the next fresh reading, and a connection missing from the
    /// last reading waits for the next fresh one.
    void reviseOwnership(std::span<const Platform::SocketTrafficSample> sockets, std::uint64_t sampleTimeNs, const ListedProcesses& listed)
    {
        for (const auto& sample : sockets)
        {
            if (sample.key == 0 || sample.pid <= 0 || !sample.readable)
            {
                continue;
            }
            const auto existing = m_Sockets.find(sample.key);
            if (existing == m_Sockets.end() || !existing->second.unowned)
            {
                continue;
            }
            const std::optional<ProcessKey> owner = listedOwner(sample, &listed);
            if (!owner.has_value())
            {
                continue;
            }
            SocketState& state = existing->second;
            if (!holdOutlasted(state, sampleTimeNs) && (state.held.received != 0 || state.held.sent != 0))
            {
                m_PendingByProcess[*owner].add(state.held);
            }
            state = SocketState{.bytesReceived = state.bytesReceived, .bytesSent = state.bytesSent, .hasBaseline = state.hasBaseline};
        }
    }

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

    struct SocketState
    {
        std::uint64_t bytesReceived = 0;
        std::uint64_t bytesSent = 0;
        bool hasBaseline = true; // False: seen only unreadable so far; the byte fields mean nothing (#1256)
        // Growth seen while unowned, held for the owner it gets later (#1259). `unowned` marks a run
        // of unowned readings that began at `unownedSinceNs` (0: time unknown); once the run outlasts
        // UNATTRIBUTED_SOCKET_HOLD_MS, `holdExpired` drops the held bytes and stops holding until the
        // connection is attributed.
        bool unowned = false;
        bool holdExpired = false;
        std::uint64_t unownedSinceNs = 0;
        Totals held{};
    };

    /// Whether `state` is in an unowned run whose hold is still live but whose deadline has passed by
    /// `sampleTimeNs`: it began more than UNATTRIBUTED_SOCKET_HOLD_MS before. An unknown time (0) at
    /// either end never ends a hold.
    [[nodiscard]] static bool holdOutlasted(const SocketState& state, std::uint64_t sampleTimeNs) noexcept
    {
        constexpr std::uint64_t HOLD_NS = static_cast<std::uint64_t>(Sampling::UNATTRIBUTED_SOCKET_HOLD_MS) * 1'000'000ULL;
        return state.unowned && !state.holdExpired && state.unownedSinceNs != 0 && sampleTimeNs > state.unownedSinceNs &&
               sampleTimeNs - state.unownedSinceNs > HOLD_NS;
    }

    /// Fold `growth` (bytes since an earlier sighting, or none) into an unowned connection's held
    /// bytes, starting its unowned run if this is its first unowned sighting, and end the hold once
    /// the run has lasted longer than an attributable connection takes to get an owner.
    static void holdUnownedGrowth(SocketState& state, const Totals& growth, std::uint64_t sampleTimeNs) noexcept
    {
        if (!state.unowned)
        {
            state.unowned = true;
            state.unownedSinceNs = sampleTimeNs;
            state.holdExpired = false;
            state.held = {};
        }
        if (state.holdExpired)
        {
            return;
        }
        if (holdOutlasted(state, sampleTimeNs))
        {
            state.holdExpired = true;
            state.held = {};
            return;
        }
        state.held.add(growth);
    }

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

    /// The process `sample`'s bytes are credited to, or nullopt if it has no owner among `listed`.
    /// The owner is the listed process with its PID, unless both start times are known and differ:
    /// that is another process that had, or took, the same PID (#1336). Without `listed`, the owner
    /// as the sample reports it (publish() matches an unknown start time by PID alone).
    [[nodiscard]] static std::optional<ProcessKey> listedOwner(const Platform::SocketTrafficSample& sample, const ListedProcesses* listed)
    {
        if (sample.pid <= 0)
        {
            return std::nullopt;
        }
        if (listed == nullptr)
        {
            return ProcessKey{.pid = sample.pid, .startTimeTicks = sample.ownerStartTimeTicks};
        }
        const auto it = listed->find(sample.pid);
        if (it == listed->end())
        {
            return std::nullopt;
        }
        if (sample.ownerStartTimeTicks != 0 && it->second != 0 && sample.ownerStartTimeTicks != it->second)
        {
            return std::nullopt;
        }
        return ProcessKey{.pid = sample.pid, .startTimeTicks = it->second};
    }

    [[nodiscard]] static constexpr std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept
    {
        constexpr auto MAX_BYTES = std::numeric_limits<std::uint64_t>::max();
        return a > MAX_BYTES - b ? MAX_BYTES : a + b;
    }

    std::unordered_map<std::uint64_t, SocketState> m_Sockets; // last reading, by connection key
    // Credited since the last publish() -- by a fresh reading, or held growth handed over by a repeated
    // one (reviseOwnership()) -- for the process with this PID and start time (start time 0: unknown,
    // matched by PID alone).
    std::unordered_map<ProcessKey, Totals, ProcessKeyHash> m_PendingByProcess;
    std::unordered_map<ProcessKey, Totals, ProcessKeyHash> m_Totals; // cumulative bytes per live process
    bool m_HasReading = false;
    std::uint64_t m_LastReadingTimeNs = 0; // sampleTimeNs of the last reading apply() folded; 0 = none
};

} // namespace Domain
