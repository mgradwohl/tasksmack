#pragma once

// Process Details' collapsible Services section (#1880): the services the selected process hosts, with
// each one's display name and state, and a link that opens it in the Services tab. On Windows they are
// the services the service list gives the process's PID (several share one svchost.exe); on Linux, the
// systemd service its cgroup is in.
//
// The view reads nothing itself. The names come with the process's snapshot (ProcessSnapshot::services,
// merged by Domain::ProcessModel, which has already ruled out a PID reused since the list was read); the
// display names and states from the service list the panel passes in. While the section is drawn open
// the panel asks for that list to be kept current (takeDrawnOpen()); ShellLayer passes that on to the
// Services panel, whose own sampler reads it at Sampling::SERVICES_REFRESH_MS.
//
// The rows are built without ImGui (Detail::hostedServiceRows()), so they are tested on their own;
// render() is run headless in tests/App/test_ProcessServicesViewRender.cpp.

#include "Domain/ServiceModel.h"
#include "Platform/IServiceProbe.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace App
{

namespace Detail
{

/// One row of the Services section.
struct HostedServiceRow
{
    std::string name;
    std::string displayName; ///< Empty when the service list does not have it (Linux: no list yet)
    Platform::ServiceState state = Platform::ServiceState::Unknown;
    bool listed = false; ///< The service list has it, so the Services tab can show it

    [[nodiscard]] friend bool operator==(const HostedServiceRow&, const HostedServiceRow&) = default;
};

/// The rows for @p names (the process's services, in its snapshot's order), their display names and
/// states taken from @p publication when it lists them. @p publication may be null.
[[nodiscard]] std::vector<HostedServiceRow> hostedServiceRows(std::span<const std::string> names,
                                                              const Domain::ServicePublication* publication);

} // namespace Detail

/// The Services section for the process Process Details shows.
class ProcessServicesView
{
  public:
    /// What render() is given about the service list.
    struct ServiceList
    {
        /// The current service list (Domain::ProcessServices::isCurrent()), or null for none.
        const Domain::ServicePublication* publication = nullptr;
        /// This platform lists services by PID (Windows): without a current list the names are not
        /// known yet, rather than none.
        bool expected = false;
    };

    /// Draws the section: a collapsing "Services" header, collapsed by default, and while it is open
    /// a table of @p names -- each listed one's name a link to the Services tab -- or a muted line
    /// for none, or for a list not read yet. Draws nothing at all when @p hasServices is false (this
    /// platform cannot tell which services a process hosts).
    /// @return The service whose link was clicked this frame, if any.
    std::optional<std::string> render(bool hasServices, std::span<const std::string> names, const ServiceList& list);

    /// Whether render() drew the section open since the last call, clearing it: the panel's per-frame
    /// update takes it, so the service list is wanted only while the section is on screen and open.
    [[nodiscard]] bool takeDrawnOpen() noexcept
    {
        const bool open = m_DrawnOpen;
        m_DrawnOpen = false;
        return open;
    }

    /// A different process was selected: its rows are built afresh.
    void onSelectionChanged() noexcept
    {
        m_RowsVersion = 0;
        m_RowsNames.clear();
        m_Rows.clear();
    }

  private:
    /// Rebuilds the rows only when the names or the service list's generation changed.
    void rebuildRowsIfStale(std::span<const std::string> names, const Domain::ServicePublication* publication);

    bool m_DrawnOpen = false;
    std::uint64_t m_RowsVersion = 0; // the service list generation m_Rows was built from (0: none)
    std::vector<std::string> m_RowsNames;
    std::vector<Detail::HostedServiceRow> m_Rows;
};

} // namespace App
