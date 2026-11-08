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

void WindowsSystemConnectionsReader::judgeRates(const EStatsSampleCounts& counts)
{
    if (m_RatesState != RatesState::Trying)
    {
        return;
    }
    switch (classifyEStatsProbe(counts, m_InconclusiveSamples))
    {
    case EStatsProbeResult::Available:
        m_RatesState = RatesState::On;
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
    }
    if (tablesRead == 0)
    {
        return {.status = SystemConnectionsStatus::Failed, .connections = {}, .detail = std::move(lastError), .ratesUnavailable = {}};
    }

    // Each holder's name and identity, from one snapshot for the whole read.
    const std::optional<std::size_t> written =
        WindowsCommitPaging::querySystemInformationInto(m_Api.ntQuerySystemInformation,
                                                        SYSTEM_PROCESS_INFORMATION_CLASS,
                                                        m_SnapshotBuffer,
                                                        WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES);
    if (written.has_value())
    {
        const std::unordered_map<std::uint32_t, SnapshotIdentity> identities =
            snapshotIdentities(std::span<const std::byte>(m_SnapshotBuffer).first(*written));
        for (SystemConnection& row : result.connections)
        {
            if (row.pid == 0)
            {
                continue; // TIME_WAIT: no process holds it
            }
            if (const auto found = identities.find(static_cast<std::uint32_t>(row.pid)); found != identities.end())
            {
                row.startTimeTicks = found->second.createTime;
                row.processName = found->second.name;
            }
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
