#pragma once

// A test-only startup selection (#1559): open Process Details for one process without any input, so
// an agent can capture its tabs with PrintWindow.
//
//   TASKSMACK_SELECT_PID=1234 ./TaskSmack                 the process with PID 1234
//   TASKSMACK_SELECT_NAME=explorer.exe ./TaskSmack        the first process with that name
//   TASKSMACK_DETAILS_TAB=gpu                             and its GPU tab (overview|gpu|network)
//
// The variables are read once, at startup (ShellLayer::onAttach() calls active()). When the PID or
// name first appears in a process snapshot, ProcessesPanel selects that process exactly as a click
// and the row menu's Details do, and Process Details brings the requested tab forward. If it has not
// appeared after MAX_SNAPSHOTS snapshots, one warning is logged and nothing happens. A PID wins over
// a name. The name match ignores case on Windows, where file names do.
//
// The parser and the matching are pure and header-only so they are unit-tested without a window
// (tests/App/test_SelectOverride.cpp); only active() reads the environment and logs.

#include "Domain/ProcessSnapshot.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace App::SelectOverride
{

inline constexpr std::string_view PID_ENV_VAR = "TASKSMACK_SELECT_PID";
inline constexpr std::string_view NAME_ENV_VAR = "TASKSMACK_SELECT_NAME";
inline constexpr std::string_view TAB_ENV_VAR = "TASKSMACK_DETAILS_TAB";
/// The top-level tab to open (system/machine, processes, details, or any registered tab's id or label).
/// It wins over the Process Details tab a TASKSMACK_SELECT_PID/_NAME selection would bring forward.
inline constexpr std::string_view MAIN_TAB_ENV_VAR = "TASKSMACK_TAB";

/// Snapshots to wait for the process before giving up (about 20 s at the default 1 s interval).
inline constexpr int MAX_SNAPSHOTS = 20;

/// The Process Details tab to bring forward.
enum class DetailsTab : std::uint8_t
{
    Overview,
    Gpu,
    Network,
};

/// The process to select: by PID when one is given, else by name.
struct Target
{
    std::optional<std::int32_t> pid;
    std::string name;
    DetailsTab tab = DetailsTab::Overview;

    [[nodiscard]] bool operator==(const Target&) const = default;
};

struct ParseResult
{
    std::optional<Target> target;      ///< nullopt: nothing to select (unset or invalid)
    std::vector<std::string> warnings; ///< why a value was ignored; empty if none was
};

namespace Detail
{

[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept
{
    const auto isSpace = [](const char c)
    {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && isSpace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] inline bool equalsIgnoreCase(const std::string_view a, const std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a,
                              b,
                              [](const char x, const char y)
                              { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace Detail

/// A positive PID of digits only that fits an int32, or nullopt.
[[nodiscard]] inline std::optional<std::int32_t> parsePid(const std::string_view text) noexcept
{
    if (text.empty() || !std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }
    std::int32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0)
    {
        return std::nullopt;
    }
    return value;
}

/// "overview", "gpu" or "network", in any case, or nullopt.
[[nodiscard]] inline std::optional<DetailsTab> parseTab(const std::string_view text) noexcept
{
    if (Detail::equalsIgnoreCase(text, "overview"))
    {
        return DetailsTab::Overview;
    }
    if (Detail::equalsIgnoreCase(text, "gpu"))
    {
        return DetailsTab::Gpu;
    }
    if (Detail::equalsIgnoreCase(text, "network"))
    {
        return DetailsTab::Network;
    }
    return std::nullopt;
}

/// Parses the three variables' values (nullptr when unset). Unset or blank values are ignored
/// quietly; an invalid PID or an unknown tab is ignored with a warning (the tab then is Overview).
[[nodiscard]] inline ParseResult parse(const char* pidValue, const char* nameValue, const char* tabValue)
{
    ParseResult result;
    const std::string_view pidText = Detail::trim(pidValue != nullptr ? pidValue : "");
    const std::string_view nameText = Detail::trim(nameValue != nullptr ? nameValue : "");
    const std::string_view tabText = Detail::trim(tabValue != nullptr ? tabValue : "");

    Target target;
    if (!pidText.empty())
    {
        target.pid = parsePid(pidText);
        if (!target.pid)
        {
            result.warnings.push_back(std::format("{}: '{}' is not a positive PID; ignored", PID_ENV_VAR, pidText));
        }
    }
    target.name = std::string(nameText);
    if (!target.pid && target.name.empty())
    {
        return result;
    }
    if (!tabText.empty())
    {
        if (const auto tab = parseTab(tabText); tab.has_value())
        {
            target.tab = *tab;
        }
        else
        {
            result.warnings.push_back(std::format("{}: '{}' is not overview, gpu or network; using overview", TAB_ENV_VAR, tabText));
        }
    }
    result.target = std::move(target);
    return result;
}

/// Whether @p snapshot is the process @p target names.
[[nodiscard]] inline bool matches(const Target& target, const Domain::ProcessSnapshot& snapshot) noexcept
{
    if (target.pid.has_value())
    {
        return snapshot.pid == *target.pid;
    }
#ifdef _WIN32
    return Detail::equalsIgnoreCase(snapshot.name, target.name);
#else
    return snapshot.name == target.name;
#endif
}

/// "PID 1234" / "'explorer.exe'"
[[nodiscard]] inline std::string describe(const Target& target)
{
    return target.pid.has_value() ? std::format("PID {}", *target.pid) : std::format("'{}'", target.name);
}

/// The selection waiting for its process: fed every snapshot generation until it fires or gives up.
class Pending
{
  public:
    /// What one snapshot generation did.
    struct Step
    {
        std::optional<std::size_t> match; ///< index of the process to select, at most once
        bool gaveUp = false;              ///< true once, when MAX_SNAPSHOTS passed without it
    };

    Pending() = default;
    explicit Pending(std::optional<Target> target) : m_Target(std::move(target))
    {}

    /// The per-frame check: false once the selection fired or gave up (or there was none).
    [[nodiscard]] bool pending() const noexcept
    {
        return m_Target.has_value();
    }

    [[nodiscard]] const std::optional<Target>& target() const noexcept
    {
        return m_Target;
    }

    /// Looks for the process in generation @p generation; a generation already seen does nothing.
    [[nodiscard]] Step onSnapshot(const std::uint64_t generation, const std::span<const Domain::ProcessSnapshot> snapshots)
    {
        if (!m_Target.has_value() || (m_SeenAny && generation == m_LastGeneration))
        {
            return {};
        }
        m_SeenAny = true;
        m_LastGeneration = generation;
        const Target& target = *m_Target;
        if (const auto it = std::ranges::find_if(snapshots, [&target](const Domain::ProcessSnapshot& s) { return matches(target, s); });
            it != snapshots.end())
        {
            m_Target.reset();
            return Step{.match = static_cast<std::size_t>(it - snapshots.begin()), .gaveUp = false};
        }
        if (++m_Snapshots >= MAX_SNAPSHOTS)
        {
            m_Target.reset();
            return Step{.match = std::nullopt, .gaveUp = true};
        }
        return {};
    }

  private:
    std::optional<Target> m_Target;
    std::uint64_t m_LastGeneration = 0;
    bool m_SeenAny = false;
    int m_Snapshots = 0;
};

/// A registered top-level tab: its event name ("Processes") and its ImGui label.
struct TabInfo
{
    std::string_view id;
    std::string_view label;
};

/// The visible text of an ImGui tab label: before "###", without a leading icon glyph and spaces.
[[nodiscard]] inline std::string_view labelText(std::string_view label) noexcept
{
    label = label.substr(0, label.find("###"));
    while (!label.empty() && (static_cast<unsigned char>(label.front()) >= 0x80 || label.front() == ' '))
    {
        label.remove_prefix(1);
    }
    return label;
}

/// The tab @p name selects, ignoring case: one whose id or label text equals it, or whose id starts
/// or ends with it ("system" -> "SystemOverview", "details" -> "ProcessDetails"). "machine" is
/// "system". nullopt when none does or @p name is blank.
[[nodiscard]] inline std::optional<std::size_t> findTab(const std::string_view name, const std::span<const TabInfo> tabs) noexcept
{
    const std::string_view wanted = Detail::equalsIgnoreCase(Detail::trim(name), "machine") ? "system" : Detail::trim(name);
    if (wanted.empty())
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < tabs.size(); ++i)
    {
        const std::string_view id = tabs[i].id;
        const bool idMatch = id.size() >= wanted.size() && (Detail::equalsIgnoreCase(id.substr(0, wanted.size()), wanted) ||
                                                            Detail::equalsIgnoreCase(id.substr(id.size() - wanted.size()), wanted));
        if (idMatch || Detail::equalsIgnoreCase(labelText(tabs[i].label), wanted))
        {
            return i;
        }
    }
    return std::nullopt;
}

/// The selection the variables ask for, read and logged on the first call; nullopt when they are
/// unset or invalid. Thread-safe.
[[nodiscard]] const std::optional<Target>& active();

} // namespace App::SelectOverride
