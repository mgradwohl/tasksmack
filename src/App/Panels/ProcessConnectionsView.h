#pragma once

// Process Details' collapsible Connections section (#799): the selected process's TCP and UDP sockets
// in a netstat-style PROTO / LOCAL ADDRESS / REMOTE ADDRESS / STATE table, sortable by any column
// (by state, then remote address, by default), with a count line above it.
//
// The view never creates a reader. The panel owns the IProcessConnectionsReader (the composition
// root's Platform::makeProcessConnectionsReader() result) and passes it to update(), which the panel
// calls from its per-frame update path, never from render(). update() starts a read only while the
// section was drawn open on the last frame, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS. The read runs on a worker thread (one at a time),
// because a read can wait on the kernel (a netlink receive is bounded at 2 s); update() never blocks
// on it and takes the result in on a later frame. render() only draws what the last read returned,
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
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

/// The status line for a read that did not succeed: its status text, and for Failed the reader's
/// @p detail when it gave one ("Could not be read: Input/output error").
[[nodiscard]] inline std::string connectionsStatusLine(Platform::ConnectionsReadStatus status, std::string_view detail)
{
    std::string line(connectionsStatusText(status));
    if (status == Platform::ConnectionsReadStatus::Failed && !detail.empty())
    {
        line += ": ";
        line += detail;
    }
    return line;
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

    /// The panel's per-frame update. Never blocks: it first takes in a read that has finished on its
    /// worker (dropped if it was for a previous selection), then starts a new one through @p reader for
    /// @p target when the section was drawn open on the last frame, no read is in flight, and a read is
    /// due -- none yet since it opened or the selection changed, or PROCESS_CONNECTIONS_REFRESH_MS since
    /// the last one started. @p reader may be null (nothing is started).
    ///
    /// While a read is in flight the reader is used only by its worker: update() does not call it, and
    /// never starts a second read. The caller must keep @p reader alive until the read finishes -- the
    /// view's destructor waits for it (one bounded read), so a reader owned alongside the view must be
    /// declared before it.
    /// @return Whether it started a read.
    bool update(Platform::IProcessConnectionsReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        takeFinishedRead(target, false);
        m_SecondsSinceRequest += deltaSeconds;
        const bool shownOpen = std::exchange(m_DrawnOpen, false);
        // Kept counting while the section is closed, so one reopened after the interval reads at once.
        if (!shownOpen || reader == nullptr || m_Pending.valid() || !reader->hasConnections())
        {
            return false;
        }
        if (m_HasRequested && (m_SecondsSinceRequest * 1000.0F) < static_cast<float>(Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS))
        {
            return false;
        }
        startRead(*reader, target);
        return true;
    }

    /// Whether a read is running on its worker.
    [[nodiscard]] bool readInFlight() const noexcept
    {
        return m_Pending.valid();
    }

    /// Waits for the read in flight, if any, and takes it in as update() would for @p target. For tests,
    /// which need a read's result deterministically; the panel never waits.
    void finishPendingRead(const Platform::ProcessTarget& target)
    {
        takeFinishedRead(target, true);
    }

    /// A different process was selected: drop its sockets, so nothing of one process is shown for the
    /// next, and read afresh when next shown. A read still in flight was for the previous process: its
    /// result is dropped when it arrives. The sort column is a viewing preference and stays.
    void onSelectionChanged() noexcept
    {
        ++m_Generation;
        m_HasRequested = false;
        m_HasRead = false;
        m_Status = Platform::ConnectionsReadStatus::Ok;
        m_Detail.clear();
        m_Rows.clear();
        m_SecondsSinceRequest = 0.0F;
        // The open frame was the previous process's: the new one's section has not been drawn yet.
        m_DrawnOpen = false;
    }

    /// Takes in a read: its status and, for Ok, its sockets formatted and sorted by the current order.
    void applyResult(const Platform::ConnectionsReadResult& result)
    {
        namespace Connections = Domain::Connections;
        m_HasRead = true;
        m_Status = result.status;
        m_Detail = result.detail;
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

    /// What failed, for a Failed read whose reader said (empty otherwise).
    [[nodiscard]] const std::string& detail() const noexcept
    {
        return m_Detail;
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
    /// Starts reading @p target through @p reader on a worker thread.
    void startRead(Platform::IProcessConnectionsReader& reader, const Platform::ProcessTarget& target)
    {
        m_HasRequested = true;
        m_SecondsSinceRequest = 0.0F;
        m_PendingGeneration = m_Generation;
        m_PendingTarget = target;
        try
        {
            m_Pending = std::async(std::launch::async, [&reader, target]() { return reader.readConnections(target); });
        }
        catch (const std::system_error& e)
        {
            // No thread to run it on: shown as a failed read, retried at the next refresh.
            applyResult({.status = Platform::ConnectionsReadStatus::Failed, .connections = {}, .detail = e.what()});
        }
    }

    /// Takes in the read in flight once it has finished (or, with @p wait, after waiting for it). Its
    /// result is applied only if it is for this selection and @p target; a stale one is dropped, and the
    /// current process is read afresh at the next chance.
    void takeFinishedRead(const Platform::ProcessTarget& target, bool wait)
    {
        if (!m_Pending.valid())
        {
            return;
        }
        if (!wait && m_Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        Platform::ConnectionsReadResult result;
        try
        {
            result = m_Pending.get();
        }
        catch (const std::exception& e)
        {
            result = {.status = Platform::ConnectionsReadStatus::Failed, .connections = {}, .detail = e.what()};
        }
        const bool current = m_PendingGeneration == m_Generation && m_PendingTarget.pid == target.pid &&
                             m_PendingTarget.startTimeTicks == target.startTimeTicks;
        if (current)
        {
            applyResult(result);
        }
        else if (m_PendingGeneration == m_Generation)
        {
            m_HasRequested = false; // read for another target without a selection change: read again
        }
    }

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

    bool m_DrawnOpen = false;    // render() drew the section open since the last update()
    bool m_HasRequested = false; // a read was started since the selection changed
    bool m_HasRead = false;      // a read was taken in since the selection changed
    float m_SecondsSinceRequest = 0.0F;
    Platform::ConnectionsReadStatus m_Status = Platform::ConnectionsReadStatus::Ok;
    std::string m_Detail;
    std::vector<Row> m_Rows;
    Detail::ConnectionsColumn m_SortColumn = Detail::ConnectionsColumn::State; // by state, then remote
    bool m_SortAscending = true;

    // The read in flight on its worker (std::async), if any, and what it was started for. A future from
    // std::async waits for its thread when destroyed, so destroying the view waits for at most one read.
    std::future<Platform::ConnectionsReadResult> m_Pending;
    std::uint64_t m_Generation = 0; // bumped by each selection change
    std::uint64_t m_PendingGeneration = 0;
    Platform::ProcessTarget m_PendingTarget{};
};

} // namespace App
