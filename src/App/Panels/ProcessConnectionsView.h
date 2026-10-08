#pragma once

// Process Details' collapsible Connections section (#799): the selected process's TCP and UDP sockets
// in a netstat-style PROTO / LOCAL ADDRESS / REMOTE ADDRESS / STATE table, sortable by any column
// (by state, then remote address, by default), with a count line above it.
//
// The view never creates a reader. The panel owns the IProcessConnectionsReader (the composition
// root's Platform::makeProcessConnectionsReader() result) and passes it to update(), which the panel
// calls from its per-frame update path, never from render(). update() reads only while the section
// was drawn open on the last frame, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS; render() only draws what the last read returned,
// formatted once per read (Domain/ProcessConnections.h), not per frame.
//
// Everything but render() is defined here, free of ImGui, so the read cadence, the statuses and the
// sort are tested against a mock reader without an ImGui context
// (tests/App/test_ProcessConnectionsView.cpp); render() is run headless in
// tests/App/test_ProcessConnectionsViewRender.cpp.

#include "Domain/ProcessConnections.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

/// Rows the table shows before it scrolls inside the section, so a server with hundreds of sockets
/// does not push the Overview's charts out of the pane.
inline constexpr std::size_t CONNECTIONS_TABLE_MAX_VISIBLE_ROWS = 12;

/// The line shown in place of the table for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view connectionsStatusText(Platform::ConnectionsReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::ConnectionsReadStatus::Ok:
        return {};
    case Platform::ConnectionsReadStatus::PermissionDenied:
        return "Not permitted (another user's process)";
    case Platform::ConnectionsReadStatus::ProcessExited:
        return "Process exited";
    case Platform::ConnectionsReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::ConnectionsReadStatus::IdentityUnknown:
        return "Not available yet"; // retried at the next refresh, once the start time is known
    case Platform::ConnectionsReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// "1 connection", "12 connections".
[[nodiscard]] inline std::string connectionsCountText(std::size_t count)
{
    return std::to_string(count) + (count == 1 ? " connection" : " connections");
}

/// The table's columns, also its sort keys (the ImGui column user IDs).
enum class ConnectionsColumn : std::uint8_t
{
    Protocol,
    Local,
    Remote,
    State,
};

} // namespace Detail

/// The Connections section for the process Process Details shows.
class ProcessConnectionsView
{
  public:
    /// One socket as the table shows it: the raw row (for sorting) and its text, formatted once per read.
    struct Row
    {
        Platform::ProcessConnection connection;
        std::string protocol; ///< "TCP", "UDP6", ...
        std::string local;    ///< "127.0.0.1:631", "[::]:22"
        std::string remote;   ///< "93.184.216.34:443", "0.0.0.0:*"
        std::string state;    ///< "ESTABLISHED", "LISTEN", "UNCONN", ...
    };

    /// Draws the section: a collapsing "Connections" header and, while it is open, the last read's
    /// count and table, or its status line. Draws nothing at all when @p hasConnections is false (the
    /// platform cannot list a process's sockets: Windows for now, synthetic runs).
    void render(bool hasConnections);

    /// The panel's per-frame update: reads @p target's sockets through @p reader when the section was
    /// drawn open on the last frame and a read is due -- none yet since it opened or the selection
    /// changed, or PROCESS_CONNECTIONS_REFRESH_MS since the last. @p reader may be null (nothing is read).
    /// @return Whether it read.
    bool update(Platform::IProcessConnectionsReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        m_SecondsSinceRead += deltaSeconds;
        const bool shownOpen = std::exchange(m_DrawnOpen, false);
        // Kept counting while the section is closed, so one reopened after the interval reads at once.
        if (!shownOpen || reader == nullptr || !reader->hasConnections())
        {
            return false;
        }
        if (m_HasRead && (m_SecondsSinceRead * 1000.0F) < static_cast<float>(Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS))
        {
            return false;
        }
        applyResult(reader->readConnections(target));
        return true;
    }

    /// A different process was selected: drop its sockets, so nothing of one process is shown for the
    /// next, and read afresh when next shown. The sort column is a viewing preference and stays.
    void onSelectionChanged() noexcept
    {
        m_HasRead = false;
        m_Status = Platform::ConnectionsReadStatus::Ok;
        m_Rows.clear();
        m_SecondsSinceRead = 0.0F;
        // The open frame was the previous process's: the new one's section has not been drawn yet.
        m_DrawnOpen = false;
    }

    /// Takes in a read: its status and, for Ok, its sockets formatted and sorted by the current order.
    void applyResult(const Platform::ConnectionsReadResult& result)
    {
        namespace Connections = Domain::Connections;
        m_HasRead = true;
        m_SecondsSinceRead = 0.0F;
        m_Status = result.status;
        m_Rows.clear();
        m_Rows.reserve(result.connections.size());
        for (const Platform::ProcessConnection& connection : result.connections)
        {
            m_Rows.push_back({.connection = connection,
                              .protocol = std::string(Connections::protocolLabel(connection.protocol, connection.family)),
                              .local = Connections::formatEndpoint(connection.family, connection.local),
                              .remote = Connections::formatEndpoint(connection.family, connection.remote),
                              .state = std::string(Connections::stateLabel(connection.protocol, connection.state))});
        }
        sortRows();
    }

    /// Records that render() drew the section open this frame (render() calls it; tests may too).
    void markDrawnOpen() noexcept
    {
        m_DrawnOpen = true;
    }

    /// Sorts by @p column, ascending or not, as clicking its header does. Ties fall back to state,
    /// then remote, then local address, then protocol, all ascending, so the order is stable across
    /// re-reads.
    void setSort(Detail::ConnectionsColumn column, bool ascending)
    {
        m_SortColumn = column;
        m_SortAscending = ascending;
        sortRows();
    }

    [[nodiscard]] Detail::ConnectionsColumn sortColumn() const noexcept
    {
        return m_SortColumn;
    }

    [[nodiscard]] bool sortAscending() const noexcept
    {
        return m_SortAscending;
    }

    /// Whether a read has been taken in since the selection changed (false: "Reading...").
    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    /// The last read's status (Ok before the first).
    [[nodiscard]] Platform::ConnectionsReadStatus status() const noexcept
    {
        return m_Status;
    }

    /// The last read's sockets, in the current sort order.
    [[nodiscard]] std::span<const Row> rows() const noexcept
    {
        return m_Rows;
    }

  private:
    /// @p a against @p b on @p column alone.
    [[nodiscard]] static std::strong_ordering compareOn(Detail::ConnectionsColumn column, const Row& a, const Row& b) noexcept
    {
        namespace Connections = Domain::Connections;
        const Platform::ProcessConnection& x = a.connection;
        const Platform::ProcessConnection& y = b.connection;
        switch (column)
        {
        case Detail::ConnectionsColumn::Protocol:
            if (const auto order = x.protocol <=> y.protocol; order != 0)
            {
                return order;
            }
            return x.family <=> y.family;
        case Detail::ConnectionsColumn::Local:
            return Connections::compareEndpoints(x.family, x.local, y.family, y.local);
        case Detail::ConnectionsColumn::Remote:
            return Connections::compareEndpoints(x.family, x.remote, y.family, y.remote);
        case Detail::ConnectionsColumn::State:
            break;
        }
        return Connections::stateSortRank(x.state) <=> Connections::stateSortRank(y.state);
    }

    void sortRows()
    {
        using Detail::ConnectionsColumn;
        const ConnectionsColumn column = m_SortColumn;
        const bool ascending = m_SortAscending;
        std::ranges::stable_sort(
            m_Rows,
            [column, ascending](const Row& a, const Row& b)
            {
                if (const auto primary = compareOn(column, a, b); primary != 0)
                {
                    return ascending ? primary < 0 : primary > 0;
                }
                for (const ConnectionsColumn tie :
                     {ConnectionsColumn::State, ConnectionsColumn::Remote, ConnectionsColumn::Local, ConnectionsColumn::Protocol})
                {
                    if (const auto order = compareOn(tie, a, b); order != 0)
                    {
                        return order < 0;
                    }
                }
                return false;
            });
    }

    void renderTable();

    bool m_DrawnOpen = false; // render() drew the section open since the last update()
    bool m_HasRead = false;
    float m_SecondsSinceRead = 0.0F;
    Platform::ConnectionsReadStatus m_Status = Platform::ConnectionsReadStatus::Ok;
    std::vector<Row> m_Rows;
    Detail::ConnectionsColumn m_SortColumn = Detail::ConnectionsColumn::State; // by state, then remote
    bool m_SortAscending = true;
};

} // namespace App
