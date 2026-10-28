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
#include <unordered_map>
#include <unordered_set>
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
    bool closed = false;  ///< Gone since the last read: shown dimmed for a moment (ConnectionChangeTracker)
};

/// Formats @p publication's sockets, in its order.
[[nodiscard]] std::vector<ConnectionRow> buildConnectionRows(const Domain::ConnectionsPublication& publication);

/// Whether @p row matches @p filter (ignoring case) in its process, PID, endpoints, protocol or state.
/// An empty filter matches everything.
[[nodiscard]] bool connectionMatches(const ConnectionRow& row, std::string_view filter);

/// Indices into @p rows matching @p filter, sorted by @p column (ties: the publication's order).
[[nodiscard]] std::vector<std::size_t>
orderConnectionRows(const std::vector<ConnectionRow>& rows, std::string_view filter, ConnectionsColumn column, bool ascending);

/// A socket's identity across generations: its protocol, family and both endpoints.
[[nodiscard]] std::uint64_t selectionKey(const Platform::SystemConnection& connection) noexcept;

/// How new and closed sockets are shown (#3066), as the Processes table shows new and exited processes
/// (#3060): a new socket's row tinted, fading out; a closed one kept for a moment, dimmed.
namespace ConnectionChanges
{
inline constexpr double NEW_SECONDS = 5.0;           ///< How long a new socket's tint takes to fade out
inline constexpr float NEW_MAX_ALPHA = 0.28F;        ///< The tint's opacity when the socket first shows
inline constexpr double CLOSED_SECONDS = 2.0;        ///< How long a closed socket's row stays
inline constexpr float CLOSED_TINT_ALPHA = 0.22F;    ///< The closed row's steady tint
inline constexpr float CLOSED_CONTENT_ALPHA = 0.55F; ///< The closed row's text opacity
} // namespace ConnectionChanges

/// Which sockets are new and which just closed, from one generation of rows to the next. Pure: the
/// times are the caller's (seconds on any steady clock).
class ConnectionChangeTracker
{
  public:
    /// The rows of a new generation, read at @p now. The first generation only records what is there,
    /// so opening the tab doesn't light up every row.
    void onGeneration(const std::vector<ConnectionRow>& current, double now);

    /// Forgets what has aged out by @p now. Returns whether a closed row went (the table's rows change).
    bool expire(double now);

    /// The tint's opacity for the socket keyed @p key at @p now; nullopt when it isn't new.
    [[nodiscard]] std::optional<float> newAlpha(std::uint64_t key, double now) const;

    /// The closed sockets still shown, oldest first, each marked closed.
    [[nodiscard]] std::vector<ConnectionRow> closedRows() const;

  private:
    struct Closed
    {
        ConnectionRow row;
        double closedAt = 0.0;
    };
    bool m_Seeded = false;
    std::unordered_map<std::uint64_t, ConnectionRow> m_Live; ///< The last generation, by key
    std::unordered_map<std::uint64_t, double> m_FirstSeen;   ///< Sockets new within NEW_SECONDS
    std::vector<Closed> m_Closed;
};

/// The table's state between frames: the rows of the generation shown, the filter and the sort.
struct ConnectionsViewState
{
    std::uint64_t builtVersion = std::numeric_limits<std::uint64_t>::max(); ///< The generation `rows` were built from
    std::vector<ConnectionRow> rows;                                        ///< The generation's sockets, then the closed ones still shown
    std::size_t liveCount = 0;                                              ///< How many of `rows` are the generation's
    std::vector<std::size_t> visible;                                       ///< Filtered and sorted
    ConnectionChangeTracker changes;                                        ///< New and closed sockets (#3066)
    std::string filter;
    std::string filterKey; ///< The filter `visible` was built for
    ConnectionsColumn sortColumn = ConnectionsColumn::Process;
    bool sortAscending = true;
    bool orderDirty = true;
    std::optional<std::uint64_t> selectedKey; ///< The selected socket (selectionKey()), kept across generations
};

/// What the user did in the table this frame.
struct ConnectionsViewAction
{
    std::int32_t pid = 0;
    std::uint64_t startTimeTicks = 0;
    bool showDetails = false; ///< Double-clicked: show the holder in Process Details too
};

/// Draws the table for @p publication (null: nothing sampled yet). Returns the holder of a row clicked
/// this frame, so the panel can select that process.
/// Brings @p state's rows (the generation's, then the closed ones still shown) and its filtered, sorted
/// order up to date with @p publication at @p now (seconds on a steady clock).
void refreshConnectionRows(const Domain::ConnectionsPublication& publication, ConnectionsViewState& state, double now);

[[nodiscard]] std::optional<ConnectionsViewAction>
renderConnectionsView(const Domain::ConnectionsPublication* publication, bool hasConnections, ConnectionsViewState& state);

} // namespace App
