#include "ConnectionsView.h"

#include "App/Panels/ProcessTableLayout.h"
#include "Domain/ConnectionsModel.h"
#include "Domain/ProcessConnections.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessConnections.h"
#include "Platform/ISystemConnections.h"
#include "UI/CellText.h"
#include "UI/EmptyState.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

constexpr const char* FILTER_HINT = ICON_FA_FILTER "  Filter by process, address, port or state";

/// "*" for an endpoint with no address and no port (a listening or unconnected socket's remote end).
[[nodiscard]] std::string endpointText(Platform::ConnectionFamily family, const Platform::ConnectionEndpoint& endpoint)
{
    const bool none = endpoint.port == 0 && std::ranges::all_of(endpoint.address, [](std::uint8_t b) { return b == 0; });
    return none ? std::string("*") : Domain::Connections::formatEndpoint(family, endpoint);
}

[[nodiscard]] std::strong_ordering compareOn(ConnectionsColumn column, const ConnectionRow& a, const ConnectionRow& b) noexcept
{
    const Platform::ProcessConnection& ca = a.connection.connection;
    const Platform::ProcessConnection& cb = b.connection.connection;
    switch (column)
    {
    case ConnectionsColumn::Process:
        return Platform::compareIgnoreCase(a.process, b.process);
    case ConnectionsColumn::Pid:
        return a.connection.pid <=> b.connection.pid;
    case ConnectionsColumn::Protocol:
        return a.protocol <=> b.protocol;
    case ConnectionsColumn::Local:
        return Domain::Connections::compareEndpoints(ca.family, ca.local, cb.family, cb.local);
    case ConnectionsColumn::Remote:
        return Domain::Connections::compareEndpoints(ca.family, ca.remote, cb.family, cb.remote);
    case ConnectionsColumn::State:
        return Domain::Connections::stateSortRank(ca.state) <=> Domain::Connections::stateSortRank(cb.state);
    }
    return std::strong_ordering::equal;
}

} // namespace

void refreshConnectionRows(const Domain::ConnectionsPublication& publication, ConnectionsViewState& state, double now)
{
    bool rebuild = false;
    if (state.builtVersion != publication.version)
    {
        state.rows = buildConnectionRows(publication);
        state.liveCount = state.rows.size();
        state.changes.onGeneration(state.rows, now);
        state.builtVersion = publication.version;
        rebuild = true;
    }
    if (state.changes.expire(now))
    {
        rebuild = true;
    }
    if (rebuild)
    {
        state.rows.resize(state.liveCount);
        std::ranges::move(state.changes.closedRows(), std::back_inserter(state.rows));
        state.orderDirty = true;
    }
    if (state.orderDirty || state.filterKey != state.filter)
    {
        state.visible = orderConnectionRows(state.rows, state.filter, state.sortColumn, state.sortAscending);
        state.filterKey = state.filter;
        state.orderDirty = false;
    }
}

void ConnectionChangeTracker::onGeneration(const std::vector<ConnectionRow>& current, double now)
{
    std::unordered_map<std::uint64_t, ConnectionRow> live;
    live.reserve(current.size());
    for (const ConnectionRow& row : current)
    {
        const std::uint64_t key = selectionKey(row.connection);
        if (m_Seeded && !m_Live.contains(key))
        {
            m_FirstSeen.emplace(key, now);
        }
        live.emplace(key, row);
    }
    if (m_Seeded)
    {
        for (auto& [key, row] : m_Live)
        {
            if (!live.contains(key))
            {
                row.closed = true;
                m_Closed.push_back({.row = std::move(row), .closedAt = now});
            }
        }
        // A socket open again with the same endpoints is live, not closed.
        std::erase_if(m_Closed, [&live](const Closed& closed) { return live.contains(selectionKey(closed.row.connection)); });
    }
    m_Live = std::move(live);
    m_Seeded = true;
}

bool ConnectionChangeTracker::expire(double now)
{
    std::erase_if(m_FirstSeen, [now](const auto& entry) { return now - entry.second >= ConnectionChanges::NEW_SECONDS; });
    return std::erase_if(m_Closed, [now](const Closed& closed) { return now - closed.closedAt >= ConnectionChanges::CLOSED_SECONDS; }) > 0;
}

std::optional<float> ConnectionChangeTracker::newAlpha(std::uint64_t key, double now) const
{
    const auto found = m_FirstSeen.find(key);
    if (found == m_FirstSeen.end())
    {
        return std::nullopt;
    }
    const double age = std::max(0.0, now - found->second);
    if (age >= ConnectionChanges::NEW_SECONDS)
    {
        return std::nullopt;
    }
    return ConnectionChanges::NEW_MAX_ALPHA * static_cast<float>(1.0 - (age / ConnectionChanges::NEW_SECONDS));
}

std::vector<ConnectionRow> ConnectionChangeTracker::closedRows() const
{
    std::vector<ConnectionRow> rows;
    rows.reserve(m_Closed.size());
    for (const Closed& closed : m_Closed)
    {
        rows.push_back(closed.row);
    }
    return rows;
}

std::vector<ConnectionRow> buildConnectionRows(const Domain::ConnectionsPublication& publication)
{
    std::vector<ConnectionRow> rows;
    rows.reserve(publication.connections.size());
    for (const Platform::SystemConnection& c : publication.connections)
    {
        const Platform::ProcessConnection& conn = c.connection;
        // "Unknown" without a holder; "?" for a holder whose name couldn't be read.
        std::string process = c.processName.empty() ? std::string("?") : c.processName;
        if (c.pid == 0)
        {
            process = "Unknown";
        }
        rows.push_back({.connection = c,
                        .process = std::move(process),
                        .pid = c.pid == 0 ? std::string() : std::to_string(c.pid),
                        .protocol = std::string(Domain::Connections::protocolLabel(conn.protocol, conn.family)),
                        .local = endpointText(conn.family, conn.local),
                        .remote = endpointText(conn.family, conn.remote),
                        .state = std::string(Domain::Connections::stateLabel(conn.protocol, conn.state))});
    }
    return rows;
}

bool connectionMatches(const ConnectionRow& row, std::string_view filter)
{
    if (filter.empty())
    {
        return true;
    }
    return Platform::containsIgnoreCase(row.process, filter) || row.pid.contains(filter) ||
           Platform::containsIgnoreCase(row.local, filter) || Platform::containsIgnoreCase(row.remote, filter) ||
           Platform::containsIgnoreCase(row.protocol, filter) || Platform::containsIgnoreCase(row.state, filter);
}

std::vector<std::size_t>
orderConnectionRows(const std::vector<ConnectionRow>& rows, std::string_view filter, ConnectionsColumn column, bool ascending)
{
    std::vector<std::size_t> order;
    order.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (connectionMatches(rows[i], filter))
        {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order,
                             [&rows, column, ascending](std::size_t a, std::size_t b)
                             {
                                 const auto primary = compareOn(column, rows[a], rows[b]);
                                 return ascending ? primary < 0 : primary > 0;
                             });
    return order;
}

std::uint64_t selectionKey(const Platform::SystemConnection& connection) noexcept
{
    // FNV-1a over the fields that tell one socket from another; a collision only moves a highlight.
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](std::uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (i * 8)) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    const Platform::ProcessConnection& c = connection.connection;
    mix(static_cast<std::uint64_t>(c.protocol));
    mix(static_cast<std::uint64_t>(c.family));
    for (const Platform::ConnectionEndpoint* endpoint : {&c.local, &c.remote})
    {
        for (const std::uint8_t byte : endpoint->address)
        {
            mix(byte);
        }
        mix(endpoint->port);
    }
    return hash;
}

namespace
{

/// One table row for @p row (table row @p i): its selectable, then its cells. The holder to select when it
/// was clicked, if any.
[[nodiscard]] std::optional<ConnectionsViewAction>
renderConnectionRow(const ConnectionRow& row, int i, const UI::ColorScheme& scheme, ConnectionsViewState& state, double now)
{
    std::optional<ConnectionsViewAction> action;
    const std::uint64_t key = selectionKey(row.connection);
    ImGui::TableNextRow();
    // A socket that just closed: dimmed with a steady tint, not selectable; one that just opened: tinted,
    // fading out (#3066), in the colours the Processes table uses for exited and new processes.
    if (row.closed)
    {
        ImVec4 tint = scheme.processExiting;
        tint.w = ConnectionChanges::CLOSED_TINT_ALPHA;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ConnectionChanges::CLOSED_CONTENT_ALPHA);
    }
    else if (const std::optional<float> alpha = state.changes.newAlpha(key, now); alpha.has_value())
    {
        ImVec4 tint = scheme.processNew;
        tint.w = *alpha;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
    }
    ImGui::TableNextColumn();
    ImGui::PushID(i);
    const bool selected = !row.closed && state.selectedKey == key;
    const ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                       (row.closed ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None);
    if (ImGui::Selectable("##row", selected, flags))
    {
        state.selectedKey = key;
        // Selecting a row selects its holder (#3066); a double-click shows it in Process Details.
        if (row.connection.pid != 0)
        {
            action = ConnectionsViewAction{.pid = row.connection.pid,
                                           .startTimeTicks = row.connection.startTimeTicks,
                                           .showDetails = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)};
        }
    }
    ImGui::PopID();
    ImGui::SameLine(0.0F, 0.0F);
    if (row.connection.pid == 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    }
    UI::Widgets::cellText(row.process);
    if (row.connection.pid == 0)
    {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("No process TaskSmack can see holds this socket: it is closing (TIME_WAIT), or it belongs to "
                              "another user's process, which only an administrator can see.");
    }
    ImGui::TableNextColumn();
    UI::Widgets::cellText(row.pid);
    ImGui::TableNextColumn();
    UI::Widgets::cellText(row.protocol);
    ImGui::TableNextColumn();
    UI::Widgets::cellText(row.local);
    ImGui::TableNextColumn();
    UI::Widgets::cellText(row.remote);
    ImGui::TableNextColumn();
    UI::Widgets::cellText(row.closed ? std::string("Closed") : row.state);
    if (row.closed)
    {
        ImGui::PopStyleVar();
    }
    return action;
}

} // namespace

std::optional<ConnectionsViewAction>
renderConnectionsView(const Domain::ConnectionsPublication* publication, bool hasConnections, ConnectionsViewState& state)
{
    if (!hasConnections)
    {
        UI::Widgets::renderEmptyState(ICON_FA_NETWORK_WIRED "  Connections aren't available",
                                      "TaskSmack lists the machine's TCP and UDP sockets on Linux. Support for Windows is planned.");
        return std::nullopt;
    }
    if (publication == nullptr || publication->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_NETWORK_WIRED "  Reading connections...");
        return std::nullopt;
    }
    if (publication->status == Platform::SystemConnectionsStatus::Failed)
    {
        const std::string detail = "The kernel's socket tables couldn't be read: " + publication->detail;
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Connections couldn't be read", detail.c_str());
        return std::nullopt;
    }

    const auto& scheme = UI::Theme::get().scheme();
    ImGui::SetNextItemWidth(ProcessTableLayout::computeFilterWidth(
        ImGui::CalcTextSize(FILTER_HINT).x, ImGui::GetStyle().FramePadding.x, ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, scheme.textMuted);
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        ImGui::InputTextWithHint("##ConnectionsFilter", FILTER_HINT, &state.filter);
    }
    ImGui::PopStyleColor();
    const double now = ImGui::GetTime();
    refreshConnectionRows(*publication, state, now);
    ImGui::SameLine();
    // Counted without the closed sockets still shown.
    const auto shownLive =
        static_cast<std::size_t>(std::ranges::count_if(state.visible, [&state](std::size_t index) { return index < state.liveCount; }));
    ImGui::TextColored(scheme.textMuted, "%zu of %zu connections", shownLive, state.liveCount);

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                            ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                            ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##ConnectionsTable", 6, TABLE_FLAGS))
    {
        return std::nullopt;
    }
    const float em = ImGui::GetFontSize();
    const auto column = [](const char* name, ImGuiTableColumnFlags flags, float width, ConnectionsColumn id)
    {
        ImGui::TableSetupColumn(name, flags, width, static_cast<ImGuiID>(id));
    };
    ImGui::TableSetupScrollFreeze(1, 1);
    column("Process", ImGuiTableColumnFlags_DefaultSort, em * 12.0F, ConnectionsColumn::Process);
    column("PID", ImGuiTableColumnFlags_None, em * 5.0F, ConnectionsColumn::Pid);
    column("Protocol", ImGuiTableColumnFlags_None, em * 5.0F, ConnectionsColumn::Protocol);
    column("Local address", ImGuiTableColumnFlags_None, em * 16.0F, ConnectionsColumn::Local);
    column("Remote address", ImGuiTableColumnFlags_None, em * 16.0F, ConnectionsColumn::Remote);
    column("State", ImGuiTableColumnFlags_None, em * 8.0F, ConnectionsColumn::State);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            state.sortColumn = static_cast<ConnectionsColumn>(specs->Specs[0].ColumnUserID);
            state.sortAscending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
            state.orderDirty = true;
        }
        specs->SpecsDirty = false;
    }
    refreshConnectionRows(*publication, state, now); // a header click may have changed the order

    std::optional<ConnectionsViewAction> action;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.visible.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            if (const auto clicked = renderConnectionRow(state.rows[state.visible[static_cast<std::size_t>(i)]], i, scheme, state, now))
            {
                action = clicked;
            }
        }
    }
    ImGui::EndTable();
    return action;
}

} // namespace App
