#pragma once

#include "Event.h"

#include <cstdint>
#include <string>
#include <utility>

namespace Core
{

/// Process selection event - emitted when user selects a process in the process list
/// Used to coordinate between ProcessesPanel and ProcessDetailsPanel without tight coupling.
/// The process is named by its exact identity, PID and start time (#1503): a PID can be reused, and
/// the uniqueKey hash of the two could collide, so neither alone tells the selected process apart.
class ProcessSelectedEvent : public Event
{
  public:
    explicit ProcessSelectedEvent(std::int32_t pid, std::uint64_t startTimeTicks) : m_Pid(pid), m_StartTimeTicks(startTimeTicks)
    {}

    [[nodiscard]] auto getPid() const -> std::int32_t
    {
        return m_Pid;
    }

    /// The process's raw start time, as Domain::ProcessSnapshot::startTimeTicks reports it.
    [[nodiscard]] auto getStartTimeTicks() const -> std::uint64_t
    {
        return m_StartTimeTicks;
    }

    EVENT_CLASS_TYPE(ProcessSelected)

  private:
    std::int32_t m_Pid;
    std::uint64_t m_StartTimeTicks;
};

/// Show-process-details request - emitted when the user asks to see a process's details from
/// somewhere other than the Process Details tab itself, e.g. the Processes table's row menu (#1209).
/// The shell switches to that tab; the process is selected by the ProcessSelectedEvent raised first.
class ShowProcessDetailsEvent : public Event
{
  public:
    ShowProcessDetailsEvent() = default;

    EVENT_CLASS_TYPE(ShowProcessDetails)
};

/// Show-service request (#1886): emitted when the user asks to see a service from elsewhere, e.g. Process
/// Details' Services section. The shell brings the Services tab forward and the Services panel selects the
/// service by name and scrolls it into view.
class ShowServiceEvent : public Event
{
  public:
    explicit ShowServiceEvent(std::string name) : m_Name(std::move(name))
    {}

    [[nodiscard]] const std::string& name() const noexcept
    {
        return m_Name;
    }

    EVENT_CLASS_TYPE(ShowService)

  private:
    std::string m_Name;
};

/// Refresh rate changed event - emitted when user changes the sampling interval
/// Allows panels with background samplers to update their refresh rates
class RefreshRateChangedEvent : public Event
{
  public:
    /// @param initial True for the configured value ShellLayer delivers once at startup (#1079):
    ///        apply it, but don't force a sample, since the models were just seeded (#1102).
    explicit RefreshRateChangedEvent(int intervalMs, bool initial = false) : m_IntervalMs(intervalMs), m_Initial(initial)
    {}

    [[nodiscard]] auto getIntervalMs() const -> int
    {
        return m_IntervalMs;
    }

    [[nodiscard]] auto isInitial() const -> bool
    {
        return m_Initial;
    }

    EVENT_CLASS_TYPE(RefreshRateChanged)

  private:
    int m_IntervalMs;
    bool m_Initial;
};

/// History duration changed event - emitted when user changes the history window (seconds)
/// Panels should adjust chart windows and domain models should update trim thresholds
class HistoryDurationChangedEvent : public Event
{
  public:
    /// @param initial True for the configured value ShellLayer delivers once at startup (#1079):
    ///        apply it, but don't force a sample (#1102).
    explicit HistoryDurationChangedEvent(int seconds, bool initial = false) : m_Seconds(seconds), m_Initial(initial)
    {}

    [[nodiscard]] auto isInitial() const -> bool
    {
        return m_Initial;
    }

    [[nodiscard]] auto getSeconds() const -> int
    {
        return m_Seconds;
    }

    EVENT_CLASS_TYPE(HistoryDurationChanged)

  private:
    int m_Seconds;
    bool m_Initial;
};

/// The Settings toggle "Count UDP traffic per process" changed (#1492, Windows): the Processes panel
/// starts or stops its UDP event trace.
class UdpEventTraceChangedEvent : public Event
{
  public:
    explicit UdpEventTraceChangedEvent(bool enabled) : m_Enabled(enabled)
    {}

    [[nodiscard]] auto isEnabled() const -> bool
    {
        return m_Enabled;
    }

    EVENT_CLASS_TYPE(UdpEventTraceChanged)

  private:
    bool m_Enabled;
};

// There is deliberately no theme-changed or font-size-changed event (#1178). Rendering is immediate
// mode and reads UI::Theme every frame, so a new theme shows on the next frame, and font-dependent
// caches already invalidate themselves by comparing the font and UI::Theme::fontGeneration().
// Such an event only ever made ProcessesPanel run an extra process enumeration for a purely visual
// change.

/// Active tab changed event - emitted when the main tab selection changes
class ActiveTabChangedEvent : public Event
{
  public:
    explicit ActiveTabChangedEvent(std::string tabName) : m_TabName(std::move(tabName))
    {}

    [[nodiscard]] auto tabName() const -> const std::string&
    {
        return m_TabName;
    }

    EVENT_CLASS_TYPE(ActiveTabChanged)

  private:
    std::string m_TabName;
};

/// Settings dialog request event - emitted when user wants to open settings
class OpenSettingsEvent : public Event
{
  public:
    OpenSettingsEvent() = default;

    EVENT_CLASS_TYPE(OpenSettings)
};

/// About dialog request event - emitted from Help's "About TaskSmack..." link and from Settings
class OpenAboutEvent : public Event
{
  public:
    OpenAboutEvent() = default;

    EVENT_CLASS_TYPE(OpenAbout)
};

/// Help window request event - emitted by F1 and the title bar's and status bar's ? buttons (#172)
class OpenHelpEvent : public Event
{
  public:
    OpenHelpEvent() = default;

    EVENT_CLASS_TYPE(OpenHelp)
};

/// Close request for the Help window, the About dialog and the Settings dialog, whichever is open
/// (TASKSMACK_DEMO's close=dialog, #2827): each layer closes its own as its close button would (Settings as
/// Cancel does, saving nothing) and lets the event on.
class CloseDialogEvent : public Event
{
  public:
    CloseDialogEvent() = default;

    EVENT_CLASS_TYPE(CloseDialog)
};

/// Elevation notice dialog request event - emitted at startup when running without elevated privileges.
/// Carries the missing permissions (Platform::MissingPrivilege bits, #2303) the Linux text names; 0 elsewhere.
class OpenElevationNoticeEvent : public Event
{
  public:
    explicit OpenElevationNoticeEvent(std::uint8_t missingPrivileges = 0) : m_MissingPrivileges(missingPrivileges)
    {}

    [[nodiscard]] std::uint8_t missingPrivileges() const noexcept
    {
        return m_MissingPrivileges;
    }

    EVENT_CLASS_TYPE(OpenElevationNotice)

  private:
    std::uint8_t m_MissingPrivileges = 0;
};

} // namespace Core
