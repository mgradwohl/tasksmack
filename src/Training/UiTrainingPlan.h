#pragma once

// The PGO UI training plan (#880): what TaskSmackUiTraining renders, for how many frames, and with
// which probes. Pure data and arithmetic, with no ImGui, so the weighting and the command line are
// unit-tested (tests/Training/test_UiTrainingPlan.cpp) apart from the driver that renders them.
//
// One run renders every workload below once, in order, each for its share of the frames. The shares
// are the weights: integers that add up to 100, so each one reads as a percentage of the run.
//
//   Workload          Tab                       Weight  Why
//   overview          System Overview           25      the default tab: every history chart, NowBars
//   processes         Processes (flat list)     18      the biggest table, sorted, with row meters
//   processes-tree    Processes (tree view)     10      the tree flatten and indent paths
//   details-overview  Process Details, Overview 14      the selected process's charts and actions block
//   details-gpu       Process Details, GPU       4      the per-process GPU charts
//   details-network   Process Details, Network   6      the connections table
//   services          Services                   8      the services table (sampled only while shown)
//   startup           Startup                    7      the startup items table
//   system            System (information)       8      the static facts page
//
// The weights follow where the time goes in an ordinary session: on the Overview and the process
// table, which are also the most expensive tabs to draw, then on a selected process, with the
// reference tabs visited least. Change them here; CONTRIBUTING.md (Profile-Guided Optimization) shows this table.
//
// A run uses one set of probes: the real ones, or the synthetic large-UI machine (#1413) with its
// history preloaded. The probe set is chosen per process (TASKSMACK_SYNTHETIC is read once), so the
// PGO scripts run the driver twice, once each way, and merge both profiles.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Training
{

/// The main tabs, by the registered ids ShellLayer gives them (its PanelTabs eventName).
enum class Tab : std::uint8_t
{
    SystemOverview,
    Processes,
    ProcessDetails,
    Services,
    Startup,
    SystemInfo,
};

/// The Process Details sub-tab a workload shows (SelectOverride::DetailsTab, without App's header).
enum class DetailsView : std::uint8_t
{
    None,
    Overview,
    Gpu,
    Network,
};

struct Workload
{
    std::string_view name;
    Tab tab;
    bool treeView = false;                   ///< Processes: the tree instead of the flat list
    DetailsView details = DetailsView::None; ///< ProcessDetails: the sub-tab to bring forward
    int weight = 0;                          ///< percent of the run's frames
};

inline constexpr std::array<Workload, 9> WORKLOADS{{
    {.name = "overview", .tab = Tab::SystemOverview, .weight = 25},
    {.name = "processes", .tab = Tab::Processes, .weight = 18},
    {.name = "processes-tree", .tab = Tab::Processes, .treeView = true, .weight = 10},
    {.name = "details-overview", .tab = Tab::ProcessDetails, .details = DetailsView::Overview, .weight = 14},
    {.name = "details-gpu", .tab = Tab::ProcessDetails, .details = DetailsView::Gpu, .weight = 4},
    {.name = "details-network", .tab = Tab::ProcessDetails, .details = DetailsView::Network, .weight = 6},
    {.name = "services", .tab = Tab::Services, .weight = 8},
    {.name = "startup", .tab = Tab::Startup, .weight = 7},
    {.name = "system", .tab = Tab::SystemInfo, .weight = 8},
}};

/// The registered id of @p tab ("SystemOverview", "Processes", ...), as ActiveTabChangedEvent carries it.
[[nodiscard]] std::string_view tabId(Tab tab) noexcept;

/// One workload's turn: @p frames frames of it.
struct Segment
{
    const Workload* workload = nullptr;
    int frames = 0;
};

/// Splits @p totalFrames between @p workloads by weight, in order: each gets floor(total * weight /
/// sum) frames, and the frames left over go one each to the largest remainders (the earlier workload
/// on a tie), so the counts add up to @p totalFrames exactly. A workload whose share rounds to zero
/// gets no segment. Empty for a non-positive total or weight sum.
[[nodiscard]] std::vector<Segment> allocateFrames(int totalFrames, std::span<const Workload> workloads);

enum class Probes : std::uint8_t
{
    Real,
    Synthetic,
};

/// The synthetic machine a --probes synthetic run shows (TASKSMACK_SYNTHETIC syntax, #1413): the
/// large-UI scenario, with the longest history preloaded.
inline constexpr std::string_view DEFAULT_SYNTHETIC_SPEC = "processes=5000,history=full";

/// 30 s at 60 fps: with both probe sets, about a minute of training per PGO run.
inline constexpr int DEFAULT_FRAMES = 1800;
inline constexpr int DEFAULT_FPS = 60;
inline constexpr int MAX_FRAMES = 1'000'000;
inline constexpr int MAX_FPS = 1000;

struct Options
{
    Probes probes = Probes::Real;
    std::string syntheticSpec{DEFAULT_SYNTHETIC_SPEC};
    int frames = DEFAULT_FRAMES;
    /// Frames are paced to this rate (the samplers run in real time, so a tab needs real time to
    /// get its data); 0 renders them back to back. The frame delta is 1/fps either way (1/60 at 0).
    int fps = DEFAULT_FPS;
    bool listOnly = false; ///< --list: print the plan and exit
    bool help = false;
};

struct ParseResult
{
    std::optional<Options> options; ///< nullopt: the command line was wrong; see error
    std::string error;
};

/// Parses the command line after the program name:
///   --probes real|synthetic   which probes (default real)
///   --synthetic SPEC          the synthetic machine; implies --probes synthetic
///   --frames N                frames in the whole run (default 1800)
///   --fps N                   pacing; 0 = unpaced (default 60)
///   --list                    print the plan and exit
///   --help
/// Both "--key value" and "--key=value" are accepted.
[[nodiscard]] ParseResult parseArguments(std::span<const std::string_view> args);

/// The usage text for --help and for a bad command line.
[[nodiscard]] std::string usage();

/// The plan, one line per segment: "overview  450 frames (25%)", and the probe set.
[[nodiscard]] std::string describePlan(const Options& options, std::span<const Segment> segments);

} // namespace Training
