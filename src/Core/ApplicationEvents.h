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
    explicit RefreshRateChangedEvent(int intervalMs) : m_IntervalMs(intervalMs)
    {}

    [[nodiscard]] auto getIntervalMs() const -> int
    {
        return m_IntervalMs;
    }

    [[nodiscard]] auto toString() const -> std::string override
    {
        return std::format("RefreshRateChangedEvent: {}ms", m_IntervalMs);
    }

    EVENT_CLASS_TYPE(RefreshRateChanged)

  private:
    int m_IntervalMs;
};

/// History duration changed event - emitted when user changes the history window (seconds)
/// Panels should adjust chart windows and domain models should update trim thresholds
class HistoryDurationChangedEvent : public Event
{
  public:
    explicit HistoryDurationChangedEvent(int seconds) : m_Seconds(seconds)
    {}

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

/// Process columns changed event - emitted when the user toggles column visibility/settings
class ProcessColumnsChangedEvent : public Event
{
  public:
    ProcessColumnsChangedEvent() = default;

    [[nodiscard]] auto toString() const -> std::string override
    {
        return "ProcessColumnsChangedEvent";
    }

    EVENT_CLASS_TYPE(ProcessColumnsChanged)
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

} // namespace Core
