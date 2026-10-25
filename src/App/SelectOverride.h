#pragma once

// A test-only startup selection (#1559): open Process Details for one process without any input, so
// an agent can capture its tabs with PrintWindow.
//
//   TASKSMACK_SELECT_PID=1234 ./TaskSmack                 the process with PID 1234
//   TASKSMACK_SELECT_NAME=explorer.exe ./TaskSmack        the first process with that name
//   TASKSMACK_DETAILS_TAB=gpu                             and its GPU tab (overview|gpu|npu|network)
//   TASKSMACK_OPEN=help                                   the Help window (help|about|settings, #172)
//   TASKSMACK_MACHINE_TAB=gpu                             the machine tab's GPU sub-tab
//                                                         (overview|cores|gpu|npu|network, #2869)
//
// TASKSMACK_DEMO (App/DemoOverride.h, #2827) makes the same requests, and more, at given times
// after startup, through the names and parsers here.
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

#include "Core/EnvUtils.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

#include <algorithm>
#include <array>
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
/// The top-level tab to open (machine/overview, processes, details, or any registered tab's id or label).
/// It wins over the Process Details tab a TASKSMACK_SELECT_PID/_NAME selection would bring forward.
inline constexpr std::string_view MAIN_TAB_ENV_VAR = "TASKSMACK_TAB";
/// The machine (hostname) tab's sub-tab to bring forward (#2869): overview, cores, gpu, npu or network.
/// It picks the sub-tab only; the main tab is TASKSMACK_TAB's (or the startup default, the machine tab).
inline constexpr std::string_view MACHINE_TAB_ENV_VAR = "TASKSMACK_MACHINE_TAB";
static_assert(Core::isKnownEnvHook(PID_ENV_VAR) && Core::isKnownEnvHook(NAME_ENV_VAR) && Core::isKnownEnvHook(TAB_ENV_VAR) &&
              Core::isKnownEnvHook(MAIN_TAB_ENV_VAR) && Core::isKnownEnvHook(MACHINE_TAB_ENV_VAR)); // listed in KNOWN_ENV_HOOKS (#2170)

/// Snapshots to wait for the process before giving up (about 20 s at the default 1 s interval).
inline constexpr int MAX_SNAPSHOTS = 20;

/// The Process Details tab to bring forward.
enum class DetailsTab : std::uint8_t
{
    Overview,
    Gpu,
    Npu, ///< Shown only on a machine with an NPU or compute accelerator (#1869); otherwise nothing is selected
    Network,
};

/// A collapsed section of Process Details' Overview, in the order the Overview draws them: what
/// TASKSMACK_DEMO's section= step expands (#2827).
enum class DetailsSection : std::uint8_t
{
    Environment, ///< Linux only
    Connections,
    Modules,
    Security, ///< Linux only
    OpenFiles,
    Services, ///< Drawn only for a process that runs a service
};

/// The dialog to open at startup (#172): "help" the Help window, "about" the About dialog, "settings" the
/// Settings dialog (#2827).
enum class StartupDialog : std::uint8_t
{
    Help,
    About,
    Settings, ///< Opened as the title bar's gear and F2 open it
};

/// A sub-tab of the machine (hostname) tab, in the order SystemMetricsPanel draws them: what
/// TASKSMACK_MACHINE_TAB and TASKSMACK_DEMO's machine= step bring forward (#2869).
enum class MachineTab : std::uint8_t
{
    Overview,
    Cores,   ///< CPU Cores: drawn once the first sample has brought per-core readings
    Gpu,     ///< Always drawn (its empty state says when there is no GPU)
    Npu,     ///< Drawn only on a machine with an NPU or compute accelerator (#1799)
    Network, ///< Network and I/O: drawn only where the platform has network counters
};

/// The names the hooks accept for each tab, section, dialog and machine sub-tab, lowercase: parseTab(),
/// parseSection(), parseDialog() and parseMachineTab() match them in any case; tabName(), sectionName(),
/// dialogName() and machineTabName() write them.
inline constexpr std::array<std::pair<std::string_view, DetailsTab>, 4> TAB_NAMES{{
    {"overview", DetailsTab::Overview},
    {"gpu", DetailsTab::Gpu},
    {"npu", DetailsTab::Npu},
    {"network", DetailsTab::Network},
}};
inline constexpr std::array<std::pair<std::string_view, MachineTab>, 5> MACHINE_TAB_NAMES{{
    {"overview", MachineTab::Overview},
    {"cores", MachineTab::Cores},
    {"gpu", MachineTab::Gpu},
    {"npu", MachineTab::Npu},
    {"network", MachineTab::Network},
}};
inline constexpr std::array<std::pair<std::string_view, DetailsSection>, 6> SECTION_NAMES{{
    {"environment", DetailsSection::Environment},
    {"connections", DetailsSection::Connections},
    {"modules", DetailsSection::Modules},
    {"security", DetailsSection::Security},
    {"openfiles", DetailsSection::OpenFiles},
    {"services", DetailsSection::Services},
}};
inline constexpr std::array<std::pair<std::string_view, StartupDialog>, 3> DIALOG_NAMES{{
    {"help", StartupDialog::Help},
    {"about", StartupDialog::About},
    {"settings", StartupDialog::Settings},
}};

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

/// The value @p text names in @p names (TAB_NAMES and the like), in any case, or nullopt.
template<typename Value, std::size_t N>
[[nodiscard]] std::optional<Value> parseName(const std::string_view text,
                                             const std::array<std::pair<std::string_view, Value>, N>& names) noexcept
{
    for (const auto& [name, value] : names)
    {
        if (Platform::equalsIgnoreCase(text, name))
        {
            return value;
        }
    }
    return std::nullopt;
}

/// The name @p names gives @p value; empty for a value not listed.
template<typename Value, std::size_t N>
[[nodiscard]] constexpr std::string_view nameOf(const Value value, const std::array<std::pair<std::string_view, Value>, N>& names) noexcept
{
    for (const auto& [name, candidate] : names)
    {
        if (candidate == value)
        {
            return name;
        }
    }
    return {};
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

/// "overview", "gpu", "npu" or "network", in any case, or nullopt.
[[nodiscard]] inline std::optional<DetailsTab> parseTab(const std::string_view text) noexcept
{
    return Detail::parseName(text, TAB_NAMES);
}

/// "modules", "openfiles", "connections", "environment", "security" or "services", in any case, or nullopt.
[[nodiscard]] inline std::optional<DetailsSection> parseSection(const std::string_view text) noexcept
{
    return Detail::parseName(text, SECTION_NAMES);
}

/// "help", "about" or "settings", in any case, or nullopt.
[[nodiscard]] inline std::optional<StartupDialog> parseDialog(const std::string_view text) noexcept
{
    return Detail::parseName(text, DIALOG_NAMES);
}

/// "overview", "cores", "gpu", "npu" or "network", in any case, or nullopt (#2869).
[[nodiscard]] inline std::optional<MachineTab> parseMachineTab(const std::string_view text) noexcept
{
    return Detail::parseName(text, MACHINE_TAB_NAMES);
}

/// The names parseTab(), parseSection(), parseDialog() and parseMachineTab() accept, for a log line:
/// "gpu", "openfiles", "about", "cores".
[[nodiscard]] constexpr std::string_view tabName(const DetailsTab tab) noexcept
{
    return Detail::nameOf(tab, TAB_NAMES);
}
[[nodiscard]] constexpr std::string_view sectionName(const DetailsSection section) noexcept
{
    return Detail::nameOf(section, SECTION_NAMES);
}
[[nodiscard]] constexpr std::string_view dialogName(const StartupDialog dialog) noexcept
{
    return Detail::nameOf(dialog, DIALOG_NAMES);
}
[[nodiscard]] constexpr std::string_view machineTabName(const MachineTab tab) noexcept
{
    return Detail::nameOf(tab, MACHINE_TAB_NAMES);
}

/// Parses the three variables' values (nullptr when unset). Unset or blank values are ignored
/// quietly; an invalid PID or an unknown tab is ignored with a warning (the tab then is Overview).
[[nodiscard]] inline ParseResult parse(const char* pidValue, const char* nameValue, const char* tabValue)
{
    ParseResult result;
    const std::string_view pidText = Platform::trimAscii(pidValue != nullptr ? pidValue : "");
    const std::string_view nameText = Platform::trimAscii(nameValue != nullptr ? nameValue : "");
    const std::string_view tabText = Platform::trimAscii(tabValue != nullptr ? tabValue : "");

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
            result.warnings.push_back(std::format("{}: '{}' is not overview, gpu, npu or network; using overview", TAB_ENV_VAR, tabText));
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

/// The ids the documented short names stand for: "machine" and "overview" the hostname tab, "details"
/// Process Details, "system" the System Information tab (#1399). Every other name must equal a tab's
/// id or visible text.
///
/// "system" is an alias and not only the System Information tab's text because the machine tab reads
/// "System" too until the System panel's first sample brings the hostname (#2731), and that tab is
/// registered first: matched by text, TASKSMACK_TAB=System opened the machine tab whenever the sample
/// landed after onAttach() (#2886). The alias names the tab whatever the machine tab reads.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> TAB_ALIASES{{
    {"machine", "SystemOverview"},
    {"overview", "SystemOverview"},
    {"details", "ProcessDetails"},
    {"system", "SystemInfo"},
}};

/// The tab @p name selects, ignoring case (tabNamesEqual()), in this order across all tabs: a tab
/// whose registered id equals it; the tab a documented alias (TAB_ALIASES) stands for, when that tab
/// is registered; a tab whose visible text equals it (the hostname, the selected process's name).
/// An id or an alias is never shadowed by another tab's text (#2886): the texts change with the
/// hostname and the selection, the ids and the aliases do not. nullopt when none matches or @p name
/// is blank.
[[nodiscard]] inline std::optional<std::size_t> findTab(const std::string_view name, const std::span<const TabInfo> tabs)
{
    const std::string_view wanted = Platform::trimAscii(name);
    if (wanted.empty())
    {
        return std::nullopt;
    }
    const auto findId = [tabs](const auto& pred) -> std::optional<std::size_t>
    {
        const auto it = std::ranges::find_if(tabs, pred);
        return it != tabs.end() ? std::optional<std::size_t>{static_cast<std::size_t>(it - tabs.begin())} : std::nullopt;
    };
    if (const auto byId = findId([wanted](const TabInfo& tab) { return tabNamesEqual(tab.id, wanted); }); byId.has_value())
    {
        return byId;
    }
    for (const auto& [alias, id] : TAB_ALIASES)
    {
        if (Platform::equalsIgnoreCase(wanted, alias))
        {
            const std::string_view target = id;
            if (const auto aliased = findId([target](const TabInfo& tab) { return tab.id == target; }); aliased.has_value())
            {
                return aliased;
            }
            break; // the alias's tab is not registered: a tab's text may still read that way
        }
    }
    return findId([wanted](const TabInfo& tab) { return tabNamesEqual(tab.text, wanted); });
}

/// What TASKSMACK_TAB resolved to.
struct MainTabChoice
{
    /// The registered id (TabInfo::id) of the tab to select; nullopt when unset, blank or unknown. An
    /// id, not an index (#1575): the tab bar finds the tab by it wherever the tab is drawn.
    std::optional<std::string> id;
    std::string warning; ///< set when a nonblank value names no tab
};

/// Resolves a TASKSMACK_TAB value (nullptr when unset) against the registered tabs.
[[nodiscard]] inline MainTabChoice resolveMainTab(const char* value, const std::span<const TabInfo> tabs)
{
    MainTabChoice choice;
    const std::string_view name = Platform::trimAscii(value != nullptr ? value : "");
    if (name.empty())
    {
        return choice;
    }
    if (const auto index = findTab(name, tabs); index.has_value())
    {
        choice.id = std::string(tabs[*index].id);
    }
    else
    {
        choice.warning = std::format("{}: '{}' names no tab; ignored", MAIN_TAB_ENV_VAR, name);
    }
    return choice;
}

/// Whether a startup selection opens Process Details: yes, unless TASKSMACK_TAB named a real tab,
/// which then wins. An unknown TASKSMACK_TAB is ignored and changes nothing.
[[nodiscard]] inline bool selectionShowsDetails(const MainTabChoice& choice) noexcept
{
    return !choice.id.has_value();
}

/// Tab-bar frames a TASKSMACK_TAB request may wait for its tab to be selected before it is dropped
/// (about 2 s at 60 fps). ImGui selects it on the second frame; this only bounds a request that can
/// never be met, so it cannot hold the tab bar on one tab for good.
inline constexpr int MAX_MAIN_TAB_FRAMES = 120;

/// The TASKSMACK_TAB selection waiting to take effect (#1575). It names the tab by its registered id
/// (PanelTabs::Tab::eventName), never by position, so it follows the tab wherever the tab bar draws
/// it. The shell submits that tab with ImGuiTabItemFlags_SetSelected on every tab-bar frame until
/// ImGui::BeginTabItem() returns true for it, and only then is the request done: ImGui applies
/// SetSelected a frame late, at the next BeginTabBar(), so a request cleared after one submission had
/// nothing to show that it took.
class PendingMainTab
{
  public:
    PendingMainTab() = default;
    explicit PendingMainTab(std::optional<std::string> id) : m_Id(std::move(id))
    {}

    /// Whether a request is still waiting.
    [[nodiscard]] bool pending() const noexcept
    {
        return m_Id.has_value();
    }

    /// The requested tab's id; nullopt once the request is done or dropped (or there was none).
    [[nodiscard]] const std::optional<std::string>& id() const noexcept
    {
        return m_Id;
    }

    /// Whether the tab registered as @p tabId is submitted with ImGuiTabItemFlags_SetSelected this frame.
    [[nodiscard]] bool wantsSelected(const std::string_view tabId) const noexcept
    {
        return m_Id.has_value() && *m_Id == tabId;
    }

    /// Reports what ImGui::BeginTabItem() returned for the tab registered as @p tabId. The request is
    /// done when its own tab reports selected; any other tab's report changes nothing.
    void onTabSubmitted(const std::string_view tabId, const bool selected) noexcept
    {
        if (selected && wantsSelected(tabId))
        {
            m_Id.reset();
        }
    }

    /// Ends one tab-bar frame. Returns the requested id once, when the request is dropped after
    /// MAX_MAIN_TAB_FRAMES frames without its tab being selected; nullopt otherwise.
    [[nodiscard]] std::optional<std::string> onFrameEnd() noexcept
    {
        if (!m_Id.has_value() || ++m_Frames < MAX_MAIN_TAB_FRAMES)
        {
            return std::nullopt;
        }
        return std::exchange(m_Id, std::nullopt);
    }

  private:
    std::optional<std::string> m_Id;
    int m_Frames = 0;
};

/// What TASKSMACK_MACHINE_TAB asked for (#2869).
struct MachineTabChoice
{
    std::optional<MachineTab> tab; ///< nullopt when unset, blank or unknown
    std::string warning;           ///< set when a nonblank value names no sub-tab
};

/// Parses a TASKSMACK_MACHINE_TAB value (nullptr when unset): "overview", "cores", "gpu", "npu" or
/// "network", in any case.
[[nodiscard]] inline MachineTabChoice parseStartupMachineTab(const char* value)
{
    MachineTabChoice choice;
    const std::string_view text = Platform::trimAscii(value != nullptr ? value : "");
    if (text.empty())
    {
        return choice;
    }
    choice.tab = parseMachineTab(text);
    if (!choice.tab.has_value())
    {
        choice.warning = std::format("{}: '{}' is not overview, cores, gpu, npu or network; ignored", MACHINE_TAB_ENV_VAR, text);
    }
    return choice;
}

/// Sampled tab-bar frames a machine sub-tab request may wait for its tab before it is dropped: the
/// same bound as a TASKSMACK_TAB request (MAX_MAIN_TAB_FRAMES), counted only on frames the sub-tabs
/// were drawn with a system snapshot, since CPU Cores and Network and I/O exist only once the first
/// sample has arrived (#2731). It bounds a request for a sub-tab this machine never draws (npu without
/// an accelerator, network without counters); one that can be met is done on its second frame.
inline constexpr int MAX_MACHINE_TAB_FRAMES = MAX_MAIN_TAB_FRAMES;

/// A machine sub-tab waiting to be brought forward (#2869): TASKSMACK_MACHINE_TAB's at startup, or
/// TASKSMACK_DEMO's machine= while running, both through SystemMetricsPanel::requestTab(). As
/// PendingMainTab, the panel submits the sub-tab with ImGuiTabItemFlags_SetSelected on every frame its
/// tab bar is drawn until ImGui::BeginTabItem() returns true for it (ImGui applies SetSelected a frame
/// late). Unlike Process Details' one-shot request, it survives frames on which the sub-tab is not
/// drawn at all -- the machine tab is not the active main tab, or the first sample hasn't brought the
/// sub-tab yet -- which is what lets a startup request reach Network and I/O.
class PendingMachineTab
{
  public:
    PendingMachineTab() = default;
    explicit PendingMachineTab(const std::optional<MachineTab> tab) noexcept : m_Tab(tab)
    {}

    /// Whether a request is still waiting.
    [[nodiscard]] bool pending() const noexcept
    {
        return m_Tab.has_value();
    }

    /// The requested sub-tab; nullopt once the request is done or dropped (or there was none).
    [[nodiscard]] const std::optional<MachineTab>& tab() const noexcept
    {
        return m_Tab;
    }

    /// Whether @p tab is submitted with ImGuiTabItemFlags_SetSelected this frame.
    [[nodiscard]] bool wantsSelected(const MachineTab tab) const noexcept
    {
        return m_Tab == tab;
    }

    /// Reports what ImGui::BeginTabItem() returned for @p tab. The request is done when its own
    /// sub-tab reports selected; any other sub-tab's report changes nothing.
    void onTabSubmitted(const MachineTab tab, const bool selected) noexcept
    {
        if (selected && wantsSelected(tab))
        {
            m_Tab.reset();
        }
    }

    /// Ends one frame on which the sub-tabs were drawn with a snapshot. Returns the requested sub-tab
    /// once, when the request is dropped after MAX_MACHINE_TAB_FRAMES such frames without its tab
    /// being selected; nullopt otherwise.
    [[nodiscard]] std::optional<MachineTab> onFrameEnd() noexcept
    {
        if (!m_Tab.has_value() || ++m_Frames < MAX_MACHINE_TAB_FRAMES)
        {
            return std::nullopt;
        }
        return std::exchange(m_Tab, std::nullopt);
    }

  private:
    std::optional<MachineTab> m_Tab;
    int m_Frames = 0;
};

/// The dialog to open at startup (#172): "help" the Help window, "about" the About dialog, "settings" the
/// Settings dialog (StartupDialog).
inline constexpr std::string_view OPEN_ENV_VAR = "TASKSMACK_OPEN";
static_assert(Core::isKnownEnvHook(OPEN_ENV_VAR)); // listed in KNOWN_ENV_HOOKS (#2170)

/// What TASKSMACK_OPEN asked for.
struct StartupDialogChoice
{
    std::optional<StartupDialog> dialog; ///< nullopt when unset, blank or unknown
    std::string warning;                 ///< set when a nonblank value names no dialog
};

/// Parses a TASKSMACK_OPEN value (nullptr when unset): "help", "about" or "settings", in any case.
[[nodiscard]] inline StartupDialogChoice parseStartupDialog(const char* value)
{
    StartupDialogChoice choice;
    const std::string_view text = Platform::trimAscii(value != nullptr ? value : "");
    if (text.empty())
    {
        return choice;
    }
    choice.dialog = parseDialog(text);
    if (!choice.dialog.has_value())
    {
        choice.warning = std::format("{}: '{}' is not help, about or settings; ignored", OPEN_ENV_VAR, text);
    }
    return choice;
}

/// Whether any test hook is set (#1569): TASKSMACK_TAB, TASKSMACK_SELECT_PID, TASKSMACK_SELECT_NAME,
/// TASKSMACK_DETAILS_TAB, TASKSMACK_OPEN or TASKSMACK_MACHINE_TAB, each nullptr when unset. A blank value
/// counts as unset, as it does everywhere else. While one is set, the startup "Limited Data" notice is
/// not queued: the hooks exist for unattended captures, and dismissing the modal needs input.
[[nodiscard]] inline bool anyTestHookActive(
    const char* mainTab, const char* pid, const char* name, const char* detailsTab, const char* open, const char* machineTab) noexcept
{
    return std::ranges::any_of(std::array{mainTab, pid, name, detailsTab, open, machineTab},
                               [](const char* value) { return value != nullptr && !Platform::trimAscii(value).empty(); });
}

/// anyTestHookActive() for the process environment, or TASKSMACK_CONFIG_DIR (#1596), TASKSMACK_SCREENSHOT
/// or TASKSMACK_DEMO (#2827) set.
[[nodiscard]] bool testHookActive();

/// The dialog TASKSMACK_OPEN asks for, read and logged on the first call. Thread-safe.
[[nodiscard]] std::optional<StartupDialog> startupDialog();

/// The selection the variables ask for, read and logged on the first call; nullopt when they are
/// unset or invalid. Thread-safe.
[[nodiscard]] const std::optional<Target>& active();

} // namespace App::SelectOverride
