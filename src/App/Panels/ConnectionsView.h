#pragma once

// The Connections tab's table (#3066): every TCP and UDP socket on the machine, with the process
// holding it, its protocol, endpoints and state, sortable and filterable. The rows are formatted once
// per published generation, never per frame. Everything but renderConnectionsView() is free of ImGui,
// so the formatting, the filter and the sort are tested without an ImGui context.

#include "Domain/ConnectionsModel.h"
#include "Platform/ISystemConnections.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

/// The table's columns, also its sort keys (the ImGui column user IDs).
enum class ConnectionsColumn : std::uint8_t
{
    Process,
    Pid,
    Protocol,
    Local,
    Remote,
    State,
};

/// One socket as the table shows it: the raw connection (for sorting) and its text.
struct ConnectionRow
{
    Platform::SystemConnection connection;
    std::string process;  ///< The holder's name, or "Unknown" (a holder TaskSmack can't see, TIME_WAIT)
    std::string pid;      ///< Empty without a holder
    std::string protocol; ///< "TCP", "UDP6"
    std::string local;    ///< "10.0.0.5:52144", "[::1]:631"
    std::string remote;   ///< "142.250.80.46:443", or "*" for none
    std::string state;    ///< "Established", "Listen"
};

/// Formats @p publication's sockets, in its order.
[[nodiscard]] std::vector<ConnectionRow> buildConnectionRows(const Domain::ConnectionsPublication& publication);

/// Whether @p row matches @p filter (ignoring case) in its process, PID, endpoints, protocol or state.
/// An empty filter matches everything.
[[nodiscard]] bool connectionMatches(const ConnectionRow& row, std::string_view filter);

/// Indices into @p rows matching @p filter, sorted by @p column (ties: the publication's order).
[[nodiscard]] std::vector<std::size_t>
orderConnectionRows(const std::vector<ConnectionRow>& rows, std::string_view filter, ConnectionsColumn column, bool ascending);

/// The table's state between frames: the rows of the generation shown, the filter and the sort.
struct ConnectionsViewState
{
    std::uint64_t builtVersion = std::numeric_limits<std::uint64_t>::max(); ///< The generation `rows` were built from
    std::vector<ConnectionRow> rows;
    std::vector<std::size_t> visible; ///< Filtered and sorted
    std::string filter;
    std::string filterKey; ///< The filter `visible` was built for
    ConnectionsColumn sortColumn = ConnectionsColumn::Process;
    bool sortAscending = true;
    bool orderDirty = true;
    std::optional<std::uint64_t> selectedKey; ///< The selected socket (selectionKey()), kept across generations
};

/// A socket's identity across generations: its protocol, family and both endpoints.
[[nodiscard]] std::uint64_t selectionKey(const Platform::SystemConnection& connection) noexcept;

/// What the user did in the table this frame.
struct ConnectionsViewAction
{
    std::int32_t pid = 0;
    std::uint64_t startTimeTicks = 0;
    bool showDetails = false; ///< Double-clicked: show the holder in Process Details too
};

/// Draws the table for @p publication (null: nothing sampled yet). Returns the holder of a row clicked
/// this frame, so the panel can select that process.
[[nodiscard]] std::optional<ConnectionsViewAction>
renderConnectionsView(const Domain::ConnectionsPublication* publication, bool hasConnections, ConnectionsViewState& state);

} // namespace App
