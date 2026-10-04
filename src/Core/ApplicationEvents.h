#pragma once

#include "Event.h"

#include <cstdint>
#include <format>

namespace Core
{

/// Process selection event - emitted when user selects a process in the process list
/// Used to coordinate between ProcessesPanel and ProcessDetailsPanel without tight coupling
class ProcessSelectedEvent : public Event
{
  public:
    explicit ProcessSelectedEvent(std::int32_t pid, uint64_t uniqueKey = 0) : m_Pid(pid), m_UniqueKey(uniqueKey)
    {}

    [[nodiscard]] auto getPid() const -> std::int32_t
    {
        return m_Pid;
    }

    [[nodiscard]] auto getUniqueKey() const -> uint64_t
    {
        return m_UniqueKey;
    }

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("ProcessSelectedEvent: pid={}, key={}", m_Pid, m_UniqueKey);
    }

    EVENT_CLASS_TYPE(ProcessSelected)

  private:
    std::int32_t m_Pid;
    uint64_t m_UniqueKey;
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

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("RefreshRateChangedEvent: {}ms", m_IntervalMs);
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

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("HistoryDurationChangedEvent: {}s", m_Seconds);
    }

    EVENT_CLASS_TYPE(HistoryDurationChanged)

  private:
    int m_Seconds;
    bool m_Initial;
};

/// Theme changed event - emitted when user selects a different UI theme
class ThemeChangedEvent : public Event
{
  public:
    explicit ThemeChangedEvent(std::string themeId) : m_ThemeId(std::move(themeId))
    {}

    [[nodiscard]] auto themeId() const -> const std::string&
    {
        return m_ThemeId;
    }

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("ThemeChangedEvent: {}", m_ThemeId);
    }

    EVENT_CLASS_TYPE(ThemeChanged)

  private:
    std::string m_ThemeId;
};

/// Font size changed event - emitted when user adjusts UI font size
class FontSizeChangedEvent : public Event
{
  public:
    explicit FontSizeChangedEvent(int size) : m_Size(size)
    {}

    [[nodiscard]] auto size() const -> int
    {
        return m_Size;
    }

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("FontSizeChangedEvent: {}", m_Size);
    }

    EVENT_CLASS_TYPE(FontSizeChanged)

  private:
    int m_Size;
};

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

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("ActiveTabChangedEvent: {}", m_TabName);
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

    [[nodiscard]] auto toString() const -> std::string override
    {
        return "OpenSettingsEvent";
    }

    EVENT_CLASS_TYPE(OpenSettings)
};

/// About dialog request event - emitted when user wants to open about/help
class OpenAboutEvent : public Event
{
  public:
    OpenAboutEvent() = default;

    [[nodiscard]] auto toString() const -> std::string override
    {
        return "OpenAboutEvent";
    }

    EVENT_CLASS_TYPE(OpenAbout)
};

/// Elevation notice dialog request event - emitted at startup when running without elevated privileges
class OpenElevationNoticeEvent : public Event
{
  public:
    OpenElevationNoticeEvent() = default;

    [[nodiscard]] auto toString() const -> std::string override
    {
        return "OpenElevationNoticeEvent";
    }

    EVENT_CLASS_TYPE(OpenElevationNotice)
};

} // namespace Core
