#pragma once

// Process Details' collapsible Security section (#1526), as a two-column table of label and value: on
// Linux the selected process's user and group IDs, supplementary groups, capability sets,
// no_new_privs, seccomp mode, LSM label and cgroup; on Windows its token's user, session, integrity
// level, elevation, AppContainer and virtualization state, privileges and groups.
//
// The view never creates a reader. The panel owns the IProcessSecurityReader (the composition root's
// Platform::makeProcessSecurityReader() result) and passes it to update(), which the panel calls from
// its per-frame update path, never from render(). update() starts a read only while the section was
// drawn open on the last frame, once when it opens or the selection changes and then every
// Domain::Sampling::PROCESS_SECURITY_REFRESH_MS, on a worker thread, one at a time
// (LazyBackgroundRead.h, as the Modules section's): on Windows naming the token's SIDs
// (LookupAccountSidW) can wait seconds each for an unreachable domain controller, so the read never
// runs on the UI thread. A read for a previous selection is dropped when it arrives; render() only
// draws the rows the last read built, or "Reading..." until the first arrives.
//
// Everything but render() is defined here, free of ImGui, so the cadence, the statuses and the rows
// are tested against a mock reader without an ImGui context; render() is run headless in
// tests/App/test_ProcessSecurityViewRender.cpp.

#include "Domain/ProcessSecurity.h"
#include "Domain/SamplingConfig.h"
#include "LazyBackgroundRead.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/ThreadName.h"

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

    // A Windows token's fields (none are set on Linux).
    const auto yesNo = [](bool value)
    {
        return std::string(value ? "Yes" : "No");
    };
    if (security.tokenUser.has_value())
    {
        rows.push_back({.label = "User", .value = Text::accountText(*security.tokenUser)});
    }
    if (security.sessionId.has_value())
    {
        rows.push_back({.label = "Session", .value = std::to_string(*security.sessionId)});
    }
    if (security.integrityLevel.has_value())
    {
        rows.push_back({.label = "Integrity level", .value = Text::integrityLevelText(*security.integrityLevel)});
    }
    if (security.elevated.has_value())
    {
        rows.push_back({.label = "Elevated", .value = yesNo(*security.elevated)});
    }
    if (security.elevationType.has_value())
    {
        rows.push_back({.label = "Elevation type", .value = std::string(Text::elevationTypeText(*security.elevationType))});
    }
    if (security.appContainer.has_value())
    {
        rows.push_back({.label = "AppContainer", .value = yesNo(*security.appContainer)});
    }
    if (security.virtualizationEnabled.has_value())
    {
        rows.push_back({.label = "Virtualization", .value = *security.virtualizationEnabled ? "Enabled" : "Disabled"});
    }
    if (security.privileges.has_value())
    {
        rows.push_back({.label = "Privileges", .value = Text::privilegeListText(*security.privileges)});
    }
    if (security.tokenGroups.has_value())
    {
        rows.push_back({.label = "Groups", .value = Text::tokenGroupListText(*security.tokenGroups)});
    }
    return rows;
}

} // namespace Detail

/// The Security section for the process Process Details shows.
class ProcessSecurityView
{
  public:
    /// Draws the section: a collapsing "Security" header and, while it is open, the last read's table
    /// or its status line ("Reading..." until the first read arrives). Draws nothing at all when
    /// @p hasSecurity is false (the platform cannot read a process's security context: synthetic runs).
    void render(bool hasSecurity);

    /// The panel's per-frame update. Never blocks: takes in a finished read for this selection and
    /// @p target, then starts the next on a worker when the section was drawn open on the last frame,
    /// none is in flight, and one is due -- none yet since it opened or the selection changed, or
    /// PROCESS_SECURITY_REFRESH_MS since the last started. @p reader may be null. The caller must keep
    /// @p reader alive until a read in flight finishes (the destructor waits for it).
    /// @return Whether it started a read.
    bool update(Platform::IProcessSecurityReader* reader, const Platform::ProcessTarget& target, float deltaSeconds)
    {
        takeFinishedRead(target, false);
        if (!m_Read.due(deltaSeconds) || reader == nullptr || !reader->hasSecurity())
        {
            return false;
        }
        if (auto failed = m_Read.start(target, [reader](const Platform::ProcessTarget& t) { return reader->readSecurity(t); }))
        {
            applyResult(*failed);
        }
        return true;
    }

    /// Waits for the read in flight, if any, and takes it in as update() would (tests only).
    void finishPendingRead(const Platform::ProcessTarget& target)
    {
        takeFinishedRead(target, true);
    }

    /// Whether a read is running on its worker.
    [[nodiscard]] bool readInFlight() const noexcept
    {
        return m_Read.inFlight();
    }

    /// A different process was selected: drop its rows, and read afresh when next shown. A read in
    /// flight is dropped when it arrives.
    void onSelectionChanged() noexcept
    {
        m_Read.reset();
        m_HasRead = false;
        m_Status = Platform::SecurityReadStatus::Ok;
        m_Detail.clear();
        m_Rows.clear();
    }

    /// Takes in a read: its status and, for Ok, the rows to show.
    void applyResult(const Platform::SecurityReadResult& result)
    {
        m_HasRead = true;
        m_Status = result.status;
        m_Detail = result.detail;
        m_Rows = (result.status == Platform::SecurityReadStatus::Ok) ? Detail::securityRows(result.security)
                                                                     : std::vector<Detail::SecurityRow>{};
    }

    /// Records that render() drew the section open this frame (render() calls it; tests may too).
    void markDrawnOpen() noexcept
    {
        m_Read.markDrawnOpen();
    }

    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    [[nodiscard]] Platform::SecurityReadStatus status() const noexcept
    {
        return m_Status;
    }

    /// For Failed, what failed, if known.
    [[nodiscard]] const std::string& detail() const noexcept
    {
        return m_Detail;
    }

    [[nodiscard]] std::span<const Detail::SecurityRow> rows() const noexcept
    {
        return m_Rows;
    }

  private:
    /// Takes in the read in flight once it has finished (or, with @p wait, after waiting for it), if it
    /// is for this selection and @p target; a stale one is dropped.
    void takeFinishedRead(const Platform::ProcessTarget& target, bool wait)
    {
        if (auto result = m_Read.takeFinished(target, wait))
        {
            applyResult(*result);
        }
    }

    /// The result shown for a read that threw, or one no worker thread could be started for.
    [[nodiscard]] static Platform::SecurityReadResult failedRead(std::string detail)
    {
        return {.status = Platform::SecurityReadStatus::Failed, .security = {}, .detail = std::move(detail)};
    }

    bool m_HasRead = false; // a read was taken in since the selection changed
    Platform::SecurityReadStatus m_Status = Platform::SecurityReadStatus::Ok;
    std::string m_Detail;
    std::vector<Detail::SecurityRow> m_Rows;

    // The read in flight and its cadence. Destroying the view waits for at most one read.
    Detail::LazyBackgroundRead<Platform::SecurityReadResult> m_Read{
        Domain::Sampling::PROCESS_SECURITY_REFRESH_MS, Platform::SECURITY_READ_THREAD_NAME, &failedRead};
};

} // namespace App
