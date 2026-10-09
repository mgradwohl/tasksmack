#pragma once

// Process Details' collapsible Security section (#1526): the selected process's user and group IDs,
// supplementary groups, capability sets, no_new_privs, seccomp mode, LSM label and cgroup, as a
// two-column table of label and value.
//
// The view never creates a reader. The panel owns the IProcessSecurityReader (the composition root's
// Platform::makeProcessSecurityReader() result) and passes it to update(), which the panel calls from
// its per-frame update path, never from render(). update() reads only while the section was drawn
// open on the last frame, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_SECURITY_REFRESH_MS; render() only draws the rows the last read built.
// The read is a few small files, so it runs on the calling thread, as the Environment section's does.
//
// Everything but render() is defined here, free of ImGui, so the cadence, the statuses and the rows
// are tested against a mock reader without an ImGui context; render() is run headless in
// tests/App/test_ProcessSecurityViewRender.cpp.

#include "Domain/ProcessSecurity.h"
#include "Domain/SamplingConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

/// The line shown in place of the table for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view securityStatusText(Platform::SecurityReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::SecurityReadStatus::Ok:
        return {};
    case Platform::SecurityReadStatus::PermissionDenied:
        return "Not readable (permission denied)";
    case Platform::SecurityReadStatus::ProcessExited:
        return "Process exited";
    case Platform::SecurityReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::SecurityReadStatus::IdentityUnknown:
        return "Not available yet"; // retried at the next refresh, once the start time is known
    case Platform::SecurityReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// One row of the Security table.
struct SecurityRow
{
    std::string label;
    std::string value;

    [[nodiscard]] friend bool operator==(const SecurityRow&, const SecurityRow&) = default;
};

/// The rows the Security table shows for @p security, in display order. A field the platform did not
/// report has no row, rather than a row claiming a default.
[[nodiscard]] inline std::vector<SecurityRow> securityRows(const Platform::ProcessSecurity& security)
{
    namespace Text = Domain::ProcessSecurity;
    std::vector<SecurityRow> rows;
    if (security.users.has_value())
    {
        rows.push_back({.label = "User", .value = Text::idSetText(*security.users)});
    }
    if (security.groups.has_value())
    {
        rows.push_back({.label = "Group", .value = Text::idSetText(*security.groups)});
    }
    if (security.users.has_value() || !security.supplementaryGroups.empty())
    {
        rows.push_back({.label = "Supplementary groups", .value = Text::groupListText(security.supplementaryGroups)});
    }
    const auto addCapabilities = [&rows](const char* label, const std::optional<std::uint64_t>& mask)
    {
        if (mask.has_value())
        {
            rows.push_back({.label = label, .value = Text::capabilitySetText(*mask)});
        }
    };
    addCapabilities("Effective capabilities", security.capabilities.effective);
    addCapabilities("Permitted capabilities", security.capabilities.permitted);
    addCapabilities("Inheritable capabilities", security.capabilities.inheritable);
    addCapabilities("Ambient capabilities", security.capabilities.ambient);
    addCapabilities("Bounding set", security.capabilities.bounding);
    if (security.noNewPrivileges.has_value())
    {
        rows.push_back({.label = "No new privileges", .value = *security.noNewPrivileges ? "Yes" : "No"});
    }
    if (security.seccomp.has_value())
    {
        rows.push_back({.label = "Seccomp", .value = std::string(Text::seccompText(*security.seccomp))});
    }
    if (!security.securityLabel.empty())
    {
        rows.push_back({.label = "Security label", .value = security.securityLabel});
    }
    if (!security.controlGroup.empty())
    {
        rows.push_back({.label = "Control group", .value = security.controlGroup});
    }
    return rows;
}

} // namespace Detail

/// The Security section for the process Process Details shows.
class ProcessSecurityView
{
  public:
    /// Draws the section: a collapsing "Security" header and, while it is open, the last read's table
    /// or its status line. Draws nothing at all when @p hasSecurity is false (the platform cannot read
    /// a process's security context: Windows for now, synthetic runs).
    void render(bool hasSecurity);

    /// The panel's per-frame update: reads @p target's security context through @p reader when the
    /// section was drawn open on the last frame and a read is due -- none yet since it opened or the
    /// selection changed, or PROCESS_SECURITY_REFRESH_MS since the last. @p reader may be null.
    /// @return Whether it read.
    bool update(Platform::IProcessSecurityReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        m_SecondsSinceRead += deltaSeconds;
        const bool shownOpen = std::exchange(m_DrawnOpen, false);
        // Kept counting while the section is closed, so one reopened after the interval reads at once.
        if (!shownOpen || reader == nullptr || !reader->hasSecurity())
        {
            return false;
        }
        if (m_HasRead && (m_SecondsSinceRead * 1000.0F) < static_cast<float>(Domain::Sampling::PROCESS_SECURITY_REFRESH_MS))
        {
            return false;
        }
        applyResult(reader->readSecurity(target));
        return true;
    }

    /// A different process was selected: drop its rows, and read afresh when next shown.
    void onSelectionChanged() noexcept
    {
        m_HasRead = false;
        m_Status = Platform::SecurityReadStatus::Ok;
        m_Detail.clear();
        m_Rows.clear();
        m_SecondsSinceRead = 0.0F;
        // The open frame was the previous process's: the new one's section has not been drawn yet.
        m_DrawnOpen = false;
    }

    /// Takes in a read: its status and, for Ok, the rows to show.
    void applyResult(const Platform::SecurityReadResult& result)
    {
        m_HasRead = true;
        m_SecondsSinceRead = 0.0F;
        m_Status = result.status;
        m_Detail = result.detail;
        m_Rows = (result.status == Platform::SecurityReadStatus::Ok) ? Detail::securityRows(result.security)
                                                                     : std::vector<Detail::SecurityRow>{};
    }

    /// Records that render() drew the section open this frame (render() calls it; tests may too).
    void markDrawnOpen() noexcept
    {
        m_DrawnOpen = true;
    }

    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    [[nodiscard]] Platform::SecurityReadStatus status() const noexcept
    {
        return m_Status;
    }

    [[nodiscard]] std::span<const Detail::SecurityRow> rows() const noexcept
    {
        return m_Rows;
    }

  private:
    bool m_HasRead = false;
    Platform::SecurityReadStatus m_Status = Platform::SecurityReadStatus::Ok;
    std::string m_Detail;
    std::vector<Detail::SecurityRow> m_Rows;
    float m_SecondsSinceRead = 0.0F;
    bool m_DrawnOpen = false; // render() drew the section open since the last update()
};

} // namespace App
