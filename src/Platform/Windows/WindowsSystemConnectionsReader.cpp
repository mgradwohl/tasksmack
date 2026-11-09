#include "WindowsSystemConnectionsReader.h"

#include "Platform/ISystemConnections.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsCommitPaging.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"
#include "Platform/Windows/WindowsProcessThreadsMath.h"
#include "Platform/Windows/WindowsSocketTables.h"
#include "Platform/Windows/WindowsTcpRows.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform::Windows
{

std::unordered_map<std::uint32_t, SnapshotIdentity> snapshotIdentities(std::span<const std::byte> snapshot)
{
    std::unordered_map<std::uint32_t, SnapshotIdentity> identities;
    // The name's pointer is an absolute address into the snapshot: checked against its bytes as integers.
    const auto base = reinterpret_cast<std::uintptr_t>(snapshot.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    std::size_t offset = 0;
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= snapshot.size())
    {
        SYSTEM_PROCESS_INFORMATION entry{};
        std::memcpy(&entry, snapshot.subspan(offset).data(), sizeof(entry));
        // A Windows PID is a HANDLE-sized integer that fits in 32 bits.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto pid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry.UniqueProcessId));
        LARGE_INTEGER createTime{};
        std::memcpy(&createTime, snapshot.subspan(offset + PROCESS_CREATE_TIME_OFFSET).data(), sizeof(createTime));
        SnapshotIdentity identity{.createTime = static_cast<std::uint64_t>(createTime.QuadPart), .name = {}};

        const auto name = reinterpret_cast<std::uintptr_t>(entry.ImageName.Buffer); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::size_t nameBytes = entry.ImageName.Length;
        if (nameBytes != 0 && name >= base && name - base <= snapshot.size() && nameBytes <= snapshot.size() - (name - base))
        {
            std::wstring text(nameBytes / sizeof(wchar_t), L'\0');
            std::memcpy(text.data(), snapshot.subspan(name - base).data(), text.size() * sizeof(wchar_t));
            identity.name = WinString::wideToUtf8(text);
        }
        identities.insert_or_assign(pid, std::move(identity));

        if (entry.NextEntryOffset == 0)
        {
            break;
        }
        offset += entry.NextEntryOffset; // re-checked against the bytes by the loop
    }
    return identities;
}

bool WindowsSystemConnectionsReader::hasConnections() const
{
    return true;
}

template<typename OwnerRowT> void WindowsSystemConnectionsReader::readCounters(const OwnerRowT& ownerRow, SystemConnection& row)
{
    if constexpr (std::is_same_v<OwnerRowT, MIB_TCPROW_OWNER_PID> || std::is_same_v<OwnerRowT, MIB_TCP6ROW_OWNER_PID>)
    {
        // Only an ESTABLISHED connection's counters are read (classifyEStatsRow()); the rest show the dash.
        if ((m_RatesState != RatesState::Trying && m_RatesState != RatesState::On) || ownerRow.dwState != TCP_STATE_ESTABLISHED)
        {
            return;
        }
        m_EStatsReads.clear();
        const std::uint64_t key = estatsConnectionKey(toConnectionEndpoints(ownerRow));
        if constexpr (std::is_same_v<OwnerRowT, MIB_TCPROW_OWNER_PID>)
        {
            MIB_TCPROW tcpRow = toTcpRow(ownerRow);
            readEStatsRow(tcpRow,
                          key,
                          ownerRow.dwOwningPid,
                          ownerRow.dwState,
                          m_Api.setPerTcpConnectionEStats,
                          m_Api.getPerTcpConnectionEStats,
                          m_EStatsEnabled,
                          m_EStatsReads,
                          m_Counts);
        }
        else
        {
            MIB_TCP6ROW tcpRow = toTcp6Row(ownerRow);
            readEStatsRow(tcpRow,
                          key,
                          ownerRow.dwOwningPid,
                          ownerRow.dwState,
                          m_Api.setPerTcp6ConnectionEStats,
                          m_Api.getPerTcp6ConnectionEStats,
                          m_EStatsEnabled,
                          m_EStatsReads,
                          m_Counts);
        }
        if (!m_EStatsReads.empty() && m_EStatsReads.back().outcome == EStatsRowOutcome::Accumulated)
        {
            row.bytesSent = m_EStatsReads.back().bytesSent;
            row.bytesReceived = m_EStatsReads.back().bytesReceived;
            row.hasByteCounters = true;
        }
    }
    else
    {
        static_cast<void>(ownerRow); // UDP: no counters
        static_cast<void>(row);
    }
}

std::optional<std::unordered_map<std::uint32_t, SnapshotIdentity>> WindowsSystemConnectionsReader::takeSnapshot()
{
    const std::optional<std::size_t> written =
        WindowsCommitPaging::querySystemInformationInto(m_Api.ntQuerySystemInformation,
                                                        SYSTEM_PROCESS_INFORMATION_CLASS,
                                                        m_SnapshotBuffer,
                                                        WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES);
    if (!written.has_value())
    {
        return std::nullopt;
    }
    return snapshotIdentities(std::span<const std::byte>(m_SnapshotBuffer).first(*written));
}

void WindowsSystemConnectionsReader::judgeRates(const EStatsSampleCounts& counts)
{
    // Every sample is judged, not just until the first success (#3573): a refusal after reads that worked (a
    // policy applied, or a driver loaded, since) is reported too.
    if (m_RatesState != RatesState::Trying && m_RatesState != RatesState::On)
    {
        return;
    }
    switch (classifyEStatsProbe(counts, m_InconclusiveSamples))
    {
    case EStatsProbeResult::Available:
        m_RatesState = RatesState::On;
        m_InconclusiveSamples = 0; // a sane read ends the streak
        break;
    case EStatsProbeResult::Unavailable:
        m_RatesState = RatesState::Refused;
        break;
    case EStatsProbeResult::Undetermined:
        // A read with no established connection proves nothing and leaves the streak as it is.
        if (counts.established > 0)
        {
            ++m_InconclusiveSamples;
        }
        break;
    }
}

SystemConnectionsRead WindowsSystemConnectionsReader::read()
{
    // Asked here, on the sampler thread, not in the constructor: the answer holds for the process's life.
    if (m_RatesState == RatesState::Unknown)
    {
        const bool elevated = m_Api.isElevated != nullptr ? m_Api.isElevated() : isProcessElevated();
        m_RatesState = elevated ? RatesState::Trying : RatesState::NeedsAdmin;
    }
    const bool readingRates = m_RatesState == RatesState::Trying || m_RatesState == RatesState::On;
    m_Counts = {};
    // The processes before the tables; the ones after are taken once they are read.
    const std::optional<std::unordered_map<std::uint32_t, SnapshotIdentity>> before = takeSnapshot();

    SystemConnectionsRead result{.status = SystemConnectionsStatus::Ok, .connections = {}, .detail = {}, .ratesUnavailable = {}};
    auto keep = [this, &result](const auto& ownerRow)
    {
        SystemConnection row{
            .connection = toConnection(ownerRow),
            .pid = static_cast<std::int32_t>(ownerRow.dwOwningPid),
            .startTimeTicks = 0,
            .processName = {},
            .bytesSent = 0,
            .bytesReceived = 0,
            .hasByteCounters = false,
            .socketInode = 0,
        };
        readCounters(ownerRow, row);
        result.connections.push_back(std::move(row));
    };
    std::size_t tablesRead = 0;
    bool tcpComplete = true;
    std::string lastError;
    for (const SocketTable table : SOCKET_TABLES)
    {
        SocketTableCopy copy = copySocketTable(m_Api.tables, table, m_TableBuffer);
        if (!copy.ok)
        {
            if (copy.error == ERROR_NOT_SUPPORTED)
            {
                ++tablesRead; // a family this machine doesn't run: complete, with no sockets
                continue;
            }
            lastError = std::move(copy.detail);
            tcpComplete = tcpComplete && table != SocketTable::Tcp4 && table != SocketTable::Tcp6;
            continue;
        }
        ++tablesRead;
        forEachSocketRow(table, m_TableBuffer, keep);
    }
    if (readingRates)
    {
        // A connection missing from a complete walk has closed: forgotten (#1418).
        m_EStatsEnabled.endSample(tcpComplete);
        judgeRates(m_Counts);
        if (m_RatesState == RatesState::Refused)
        {
            // Refused this read: no row shows counters beside the reason.
            for (SystemConnection& row : result.connections)
            {
                row.hasByteCounters = false;
                row.bytesSent = 0;
                row.bytesReceived = 0;
            }
        }
    }
    if (tablesRead == 0)
    {
        return {.status = SystemConnectionsStatus::Failed, .connections = {}, .detail = std::move(lastError), .ratesUnavailable = {}};
    }

    // Each holder's name and identity, from the snapshots either side of the tables (#3573): a PID whose process
    // is the same one in both (same creation time) held its sockets throughout, since a PID is reused only once
    // its process is gone. One that changed, or is missing from either, may name a stranger: such a row is
    // nobody's (PID 0), so it selects nothing. With either snapshot unreadable, the PIDs stay, unnamed.
    if (const std::optional<std::unordered_map<std::uint32_t, SnapshotIdentity>> after = takeSnapshot();
        before.has_value() && after.has_value())
    {
        for (SystemConnection& row : result.connections)
        {
            if (row.pid == 0)
            {
                continue; // TIME_WAIT: no process holds it
            }
            const auto pid = static_cast<std::uint32_t>(row.pid);
            const auto then = before->find(pid);
            const auto now = after->find(pid);
            if (then == before->end() || now == after->end() || then->second.createTime != now->second.createTime)
            {
                row.pid = 0; // it exited, or its PID was reused, while the tables were read
                continue;
            }
            row.startTimeTicks = now->second.createTime;
            row.processName = now->second.name;
        }
    }

    if (m_RatesState == RatesState::NeedsAdmin)
    {
        result.ratesUnavailable = RATES_NEED_ADMINISTRATOR;
    }
    else if (m_RatesState == RatesState::Refused)
    {
        result.ratesUnavailable = RATES_REFUSED;
    }
    return result;
}

} // namespace Platform::Windows
