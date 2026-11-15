// LinuxProcessProbe's socket traffic (#1101, #3366): TCP from sock_diag, UDP from the collector, and the socket-inode map, split from
// LinuxProcessProbe.cpp (#3772).

// Linux only, as LinuxProcessProbe.cpp is (parseable elsewhere, e.g. Windows clangd).
#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<pwd.h>) && __has_include(<unistd.h>)

#include "Domain/SamplingConfig.h"
#include "LinuxProcessProbe.h"
#include "NetlinkSocketStats.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/PlatformConfig.h"
#include "Platform/ProcessTypes.h"
#include "Platform/UdpTrafficSamples.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/syscall.h> // NOLINT(misc-include-cleaner): SYS_ioprio_get, which include-cleaner has no mapping for

namespace Platform
{

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
SocketTrafficReading LinuxProcessProbe::readSocketTraffic() const
{
    if (!m_HasNetworkCounters)
    {
        return {};
    }
    // Copy out a stable local reference: if setSocketStatsCacheTtl() swaps m_SocketStats
    // concurrently, this call keeps using the instance it started with (kept alive by
    // this shared_ptr) rather than racing the reassignment.
    const auto stats = socketStats();
    if (!stats)
    {
        return {};
    }

    // Query all TCP sockets with their byte counters. A failed or partial dump comes back empty
    // with sampledAt unset (#1160) and is reported as no reading (sampleTimeNs 0), not as an empty
    // one: a socket missing from it would look closed and then, back in the next reading, new.
    // The query is cached (DEFAULT_SOCKET_STATS_CACHE_TTL), so the same reading -- with the same
    // sampledAt -- can come back for several calls; Domain folds it once.
    std::chrono::steady_clock::time_point sampledAt;
    const std::vector<SocketStats> sockets = stats->queryAllSockets(&sampledAt);
    if (sampledAt == std::chrono::steady_clock::time_point{})
    {
        return {};
    }

    SocketTrafficReading reading;
    reading.sampleTimeNs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sampledAt.time_since_epoch()).count());
    if (sockets.empty())
    {
        // A complete reading with no sockets: every connection closed, so none is unowned any more.
        // Forget them: a later connection reusing one of their inodes would otherwise inherit its
        // first-seen time, look older than the map, and not trigger the early rebuild (#1327 review).
        // A failed reading (returned above) leaves them alone: its sockets are unknown, not closed.
        {
            const std::scoped_lock lock{m_UnownedSocketsMutex};
            m_UnownedSocketsFirstSeen.clear();
        }
        appendUdpTraffic(reading); // the UDP map walk, never under the lock (#3860), as on the main path
        return reading;
    }

    // Attribute each socket to the process holding it (socket inode -> PID, from /proc/[pid]/fd).
    // A socket not in the map (opened since its last rebuild, or held by a process we can't read)
    // is still reported, unattributed, so Domain tracks its counters from now on.
    // The owner comes with its start time, so Domain can tell it from a process that reused its PID (#1336).
    const auto ownerOf = [](const InodeToPidMap* inodeToPid, std::uint64_t inode) -> SocketOwner
    {
        if (inodeToPid != nullptr)
        {
            if (const auto it = inodeToPid->find(inode); it != inodeToPid->end())
            {
                return it->second;
            }
        }
        return {};
    };
    auto snapshot = currentInodeToPidMap(std::chrono::milliseconds{Domain::Sampling::INODE_PID_CACHE_TTL_MS});

    bool unownedSinceBuild = false;
    {
        const std::scoped_lock lock{m_UnownedSocketsMutex};
        // A socket with no owner that wasn't already unowned in a reading taken before the map was
        // built may have been opened since the build: rebuild early (rate-limited) so a new
        // connection is attributed in the reading it first appears in, rather than up to a TTL later
        // with its first bytes, or all of a short one's, never credited (#1259). Sockets held by
        // processes we can't read stay unowned through the rebuild and so don't trigger another.
        // The rebuild may land on a cached socket query (same sampledAt): the reading then differs
        // from the last only in ownership, which Domain still applies (SocketTrafficAccumulator).
        unownedSinceBuild = std::ranges::any_of(sockets,
                                                [&](const SocketStats& socket)
                                                {
                                                    if (ownerOf(snapshot.map.get(), socket.inode).pid != 0)
                                                    {
                                                        return false;
                                                    }
                                                    const auto seen = m_UnownedSocketsFirstSeen.find(socket.inode);
                                                    const auto firstSeen =
                                                        (seen != m_UnownedSocketsFirstSeen.end()) ? seen->second : sampledAt;
                                                    return firstSeen > snapshot.builtAt;
                                                });
    }
    // The early rebuild may scan every /proc/[pid]/fd: do it without holding m_UnownedSocketsMutex.
    if (unownedSinceBuild)
    {
        snapshot = currentInodeToPidMap(m_InodeMapEarlyRebuildInterval);
    }

    {
        const std::scoped_lock lock{m_UnownedSocketsMutex};
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> unowned;
        reading.sockets.reserve(sockets.size());
        for (const auto& socket : sockets)
        {
            const SocketOwner owner = ownerOf(snapshot.map.get(), socket.inode);
            if (owner.pid == 0)
            {
                const auto seen = m_UnownedSocketsFirstSeen.find(socket.inode);
                unowned.insert_or_assign(socket.inode, (seen != m_UnownedSocketsFirstSeen.end()) ? seen->second : sampledAt);
            }
            reading.sockets.push_back(SocketTrafficSample{
                .key = socket.inode,
                .pid = owner.pid,
                .ownerStartTimeTicks = owner.startTimeTicks,
                .bytesReceived = socket.bytesReceived,
                .bytesSent = socket.bytesSent,
            });
        }
        m_UnownedSocketsFirstSeen = std::move(unowned);
    }
    appendUdpTraffic(reading);
    return reading;
}

void LinuxProcessProbe::appendUdpTraffic(SocketTrafficReading& reading) const
{
    const std::shared_ptr<IUdpTrafficCollector> udp = udpCollector();
    if (udp == nullptr)
    {
        return;
    }
    // Sampled once per reading Domain folds in: a cached TCP reading comes back with the same time, and Domain skips
    // it, so a UDP sample taken for it (a run's seeding reading, say) would be lost. It gets the last rows instead.
    {
        const std::scoped_lock lock(m_UdpCollectorMutex);
        if (reading.sampleTimeNs != 0 && reading.sampleTimeNs == m_UdpReadingTimeNs)
        {
            UdpTraffic::appendSamples(reading.sockets, m_UdpRows);
            return;
        }
    }
    std::vector<UdpProcessTraffic> traffic = udp->sample(); // a map walk: not under the lock
    UdpTraffic::appendSamples(reading.sockets, traffic);
    const std::scoped_lock lock(m_UdpCollectorMutex);
    m_UdpReadingTimeNs = reading.sampleTimeNs;
    m_UdpRows = std::move(traffic);
}

void LinuxProcessProbe::setUdpTrafficCollector(const std::shared_ptr<IUdpTrafficCollector>& collector)
{
    const std::scoped_lock lock(m_UdpCollectorMutex);
    m_UdpCollector = collector;
}

std::shared_ptr<IUdpTrafficCollector> LinuxProcessProbe::udpCollector() const
{
    const std::scoped_lock lock(m_UdpCollectorMutex);
    return m_UdpCollector;
}

std::pair<LinuxProcessProbe::InodeToPidSnapshot, bool> LinuxProcessProbe::claimInodeMapRebuild(std::chrono::milliseconds maxAge) const
{
    // The rebuild slot is claimed by advancing m_InodeToPidCacheTime under the lock, so only one
    // thread rebuilds per TTL window while all others continue using the previous shared_ptr
    // snapshot (see #460).
    const std::scoped_lock lock{m_InodePidCacheMutex};
    const auto now = std::chrono::steady_clock::now();
    // However the map got stale, never scan more often than the early-rebuild interval: an empty
    // scan backdates m_InodeToPidCacheTime for a quick retry, and that retry must not combine
    // with an early rebuild into two scans per reading.
    const bool claimed = (now - m_InodeToPidCacheTime) >= maxAge && (now - m_InodeToPidLastAttempt) >= m_InodeMapEarlyRebuildInterval;
    if (claimed)
    {
        // Advance the timestamps now so any other thread that checks while we are scanning /proc sees
        // a fresh time and skips rebuilding.
        m_InodeToPidCacheTime = now;
        m_InodeToPidLastAttempt = now;
    }
    return {InodeToPidSnapshot{.map = m_InodeToPidCache, .builtAt = m_InodeToPidBuiltAt}, claimed}; // current (possibly stale) snapshot
}

LinuxProcessProbe::InodeToPidSnapshot LinuxProcessProbe::publishInodeToPidMap(InodeToPidMap rebuilt,
                                                                              std::chrono::steady_clock::time_point scanStart) const
{
    // Allocated outside the lock; readers keep using the old snapshot until the swap.
    auto map = rebuilt.empty() ? nullptr : std::make_shared<const InodeToPidMap>(std::move(rebuilt));
    const std::scoped_lock lock{m_InodePidCacheMutex};
    if (map)
    {
        m_InodeToPidCache = std::move(map);
        m_InodeToPidCacheTime = std::chrono::steady_clock::now();
    }
    else
    {
        // Preserve the previous snapshot when procfs enumeration transiently
        // produces no entries; allow a quick retry instead of waiting the full TTL
        // (still no sooner than m_InodeMapEarlyRebuildInterval after this attempt).
        constexpr auto EMPTY_REBUILD_RETRY_MS = std::chrono::milliseconds{100};
        const auto retryDelay = std::min(EMPTY_REBUILD_RETRY_MS, m_InodeMapTtl);
        m_InodeToPidCacheTime = std::chrono::steady_clock::now() - (m_InodeMapTtl - retryDelay);
        // The scan still tried to resolve every socket unowned before it: advance builtAt so
        // those sockets don't count as opened since the build and trigger an early rebuild
        // every interval (#1259).
    }
    m_InodeToPidBuiltAt = scanStart;
    return {.map = m_InodeToPidCache, .builtAt = m_InodeToPidBuiltAt};
}

LinuxProcessProbe::InodeToPidSnapshot LinuxProcessProbe::currentInodeToPidMap(std::chrono::milliseconds maxAge) const
{
    // Refresh inode-to-PID map on a TTL basis to avoid scanning /proc/[pid]/fd/* for every reading.
    // Normally enumerate() has just rebuilt it from its own fd walk (#1426), and this only rebuilds
    // it early (a socket that appeared unowned) or for a reading taken without an enumerate().
    auto [snapshot, claimed] = claimInodeMapRebuild(maxAge);
    if (!claimed)
    {
        return snapshot;
    }
    // Build the map outside the lock; concurrent threads keep using the old snapshot.
    const auto scanStart = std::chrono::steady_clock::now();
    if (m_InodeMapScanHook)
    {
        m_InodeMapScanHook();
    }
    return publishInodeToPidMap(buildInodeToPidMap(m_ProcRoot), scanStart);
}

#endif // TASKSMACK_HAS_NETLINK_SOCKET_STATS

} // namespace Platform

#endif
