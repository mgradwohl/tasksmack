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
// a name. The name match ignores case on Windows the way file names do (CompareStringOrdinal), and
// is exact elsewhere.
//
// The parser and the matching are header-only and unit-tested without a window
// (tests/App/test_SelectOverride.cpp); SelectOverride.cpp holds the platform name comparisons and
// active(), the only part that reads the environment and logs.

#include "Domain/ProcessSnapshot.h"

#include <algorithm>
#include <array>
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
#include <utility>
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

/// Whether two process names are the same: ignoring case as Windows compares file names (ordinal,
/// on UTF-16, so "É.exe" is "é.exe") on Windows; exactly elsewhere, where file names are case-sensitive.
[[nodiscard]] bool processNamesEqual(std::string_view a, std::string_view b);

/// Whether two tab names are the same, ignoring case: as processNamesEqual() on Windows; for ASCII
/// letters only elsewhere.
[[nodiscard]] bool tabNamesEqual(std::string_view a, std::string_view b);

/// Whether @p snapshot is the process @p target names.
[[nodiscard]] inline bool matches(const Target& target, const Domain::ProcessSnapshot& snapshot)
{
    if (target.pid.has_value())
    {
        return snapshot.pid == *target.pid;
    }
    return processNamesEqual(snapshot.name, target.name);
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

    /// The Process Details tab to bring forward, once, after the selection fired; nullopt before
    /// that, after it was taken, and always when the selection gave up, so no stale request is left.
    [[nodiscard]] std::optional<DetailsTab> takeFiredTab() noexcept
    {
        return std::exchange(m_FiredTab, std::nullopt);
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
            m_FiredTab = target.tab;
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
    std::optional<DetailsTab> m_FiredTab;
    std::uint64_t m_LastGeneration = 0;
    bool m_SeenAny = false;
    int m_Snapshots = 0;
};

/// A registered top-level tab: its event name ("Processes") and its label's visible text, unescaped
/// (the hostname, "Processes", the process name), as PanelTabs::Tab::text gives it.
struct TabInfo
{
    std::string_view id;
    std::string_view text;
};

/// The ids the documented short names stand for: "system" and "machine" the system tab, "details"
/// Process Details. Every other name must equal a tab's id or text.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 3> TAB_ALIASES{{
    {"system", "SystemOverview"},
    {"machine", "SystemOverview"},
    {"details", "ProcessDetails"},
}};

/// The tab @p name selects, ignoring case (tabNamesEqual()): first a tab whose registered id or
/// visible text equals it, across all tabs; then a documented alias (TAB_ALIASES). nullopt when
/// none does or @p name is blank.
[[nodiscard]] inline std::optional<std::size_t> findTab(const std::string_view name, const std::span<const TabInfo> tabs)
{
    const std::string_view wanted = Detail::trim(name);
    if (wanted.empty())
    {
        return std::nullopt;
    }
    const auto findId = [tabs](const auto& pred) -> std::optional<std::size_t>
    {
        const auto it = std::ranges::find_if(tabs, pred);
        return it != tabs.end() ? std::optional<std::size_t>{static_cast<std::size_t>(it - tabs.begin())} : std::nullopt;
    };
    if (const auto exact =
            findId([wanted](const TabInfo& tab) { return tabNamesEqual(tab.id, wanted) || tabNamesEqual(tab.text, wanted); });
        exact.has_value())
    {
        return exact;
    }
    for (const auto& [alias, id] : TAB_ALIASES)
    {
        if (Detail::equalsIgnoreCase(wanted, alias))
        {
            const std::string_view target = id;
            return findId([target](const TabInfo& tab) { return tab.id == target; });
        }
    }
    return std::nullopt;
}

/// What TASKSMACK_TAB resolved to.
struct MainTabChoice
{
    std::optional<std::size_t> index; ///< the tab to select; nullopt when unset, blank or unknown
    std::string warning;              ///< set when a nonblank value names no tab
};

/// Resolves a TASKSMACK_TAB value (nullptr when unset) against the registered tabs.
[[nodiscard]] inline MainTabChoice resolveMainTab(const char* value, const std::span<const TabInfo> tabs)
{
    MainTabChoice choice;
    const std::string_view name = Detail::trim(value != nullptr ? value : "");
    if (name.empty())
    {
        return choice;
    }
    choice.index = findTab(name, tabs);
    if (!choice.index)
    {
        choice.warning = std::format("{}: '{}' names no tab; ignored", MAIN_TAB_ENV_VAR, name);
    }
    return choice;
}

/// Whether a startup selection opens Process Details: yes, unless TASKSMACK_TAB named a real tab,
/// which then wins. An unknown TASKSMACK_TAB is ignored and changes nothing.
[[nodiscard]] constexpr bool selectionShowsDetails(const MainTabChoice& choice) noexcept
{
    return !choice.index.has_value();
}

/// Whether any test hook is set (#1569): TASKSMACK_TAB, TASKSMACK_SELECT_PID, TASKSMACK_SELECT_NAME or
/// TASKSMACK_DETAILS_TAB, each nullptr when unset. A blank value counts as unset, as it does everywhere
/// else. While one is set, the startup "Limited Data" notice is not queued: the hooks exist for
/// unattended captures, and dismissing the modal needs input.
[[nodiscard]] inline bool anyTestHookActive(const char* mainTab, const char* pid, const char* name, const char* detailsTab) noexcept
{
    return std::ranges::any_of(std::array{mainTab, pid, name, detailsTab},
                               [](const char* value) { return value != nullptr && !Detail::trim(value).empty(); });
}

/// anyTestHookActive() for the process environment.
[[nodiscard]] bool testHookActive();

/// The selection the variables ask for, read and logged on the first call; nullopt when they are
/// unset or invalid. Thread-safe.
[[nodiscard]] const std::optional<Target>& active();

} // namespace App::SelectOverride
