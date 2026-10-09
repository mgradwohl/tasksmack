#pragma once

// The synthetic large-UI scenario (#1413): an opt-in, deterministic stand-in machine for
// limit-setting captures (frame time, vertex counts, PGO training, idle CPU at the limits).
//
//   TASKSMACK_SYNTHETIC=processes=5000,history=full ./TaskSmack
//
// When set, the App composition root builds the panels' models on Platform::Synthetic probes instead
// of the real ones, preloads their histories to the full window at the fastest refresh interval, and
// refuses every process action. The variable is read once, at startup; when it is unset (or "0",
// "off", "false", "no") nothing synthetic is created and every probe comes from Platform::make*().
//
// Keys (comma-separated key=value; a bare "1"/"on" takes every default):
//   processes=N   process count (default 2000)
//   cores=N       logical CPUs (default 16)
//   disks=N       disks (default 4)
//   interfaces=N  network interfaces (default 4)
//   seed=N        generator seed (default 1413): the same seed gives the same machine
//   history=full|none|S  history to preload: the longest window (default), none, or S seconds
//   refresh=MS    refresh interval to start at, instead of the configured one
// The history and refresh overrides apply to this run only; the config file is not changed.

#include "Domain/ProcessModel.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/Synthetic/SyntheticWorkload.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App::Synthetic
{

inline constexpr std::string_view ENV_VAR = "TASKSMACK_SYNTHETIC";

struct ScenarioConfig
{
    Platform::Synthetic::WorkloadSpec workload;
    /// History preloaded at startup, seconds; 0 = none. Within SamplingConfig's history range.
    int historySeconds = 0;
    /// Refresh interval to start at (ms), within SamplingConfig's range; nullopt = the configured one.
    std::optional<int> refreshIntervalMs;
};

struct ParseResult
{
    std::optional<ScenarioConfig> config; ///< nullopt: the scenario is off
    std::vector<std::string> warnings;    ///< keys or values that were ignored or clamped
};

/// Parses a TASKSMACK_SYNTHETIC value. nullptr, empty and the "off" flag words turn it off; any other
/// value turns it on, with the defaults for every key it doesn't set (or sets badly, with a warning).
[[nodiscard]] ParseResult parseScenario(const char* value);

/// "processes=2000 cores=16 disks=4 interfaces=4 seed=1413 history=1800s refresh=configured"
[[nodiscard]] std::string describe(const ScenarioConfig& config);

/// An active scenario: its settings and the one synthetic machine every probe made from it shares.
class Scenario
{
  public:
    explicit Scenario(const ScenarioConfig& config);
    Scenario(const ScenarioConfig& config, std::shared_ptr<const Platform::Synthetic::Workload> workload);

    [[nodiscard]] const ScenarioConfig& config() const noexcept
    {
        return m_Config;
    }
    [[nodiscard]] const std::shared_ptr<const Platform::Synthetic::Workload>& workload() const noexcept
    {
        return m_Workload;
    }
    /// Intervals of history the preload provides: historySeconds at REFRESH_INTERVAL_MIN_MS
    /// (18000 for history=full).
    [[nodiscard]] std::size_t historySamples() const noexcept;

  private:
    ScenarioConfig m_Config;
    std::shared_ptr<const Platform::Synthetic::Workload> m_Workload;
};

/// The scenario TASKSMACK_SYNTHETIC selects, read and logged on the first call (main() makes it at
/// startup); nullptr when it is off. Thread-safe.
[[nodiscard]] const Scenario* activeScenario();

// Composition: with a scenario, its synthetic probe; without one (nullptr), the platform's own.
// There is no synthetic power or GPU probe: with a scenario those are nullptr (no battery, no GPUs),
// so nothing real is mixed into the synthetic machine.

[[nodiscard]] std::unique_ptr<Platform::IProcessProbe> makeProcessProbe(const Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::IProcessActions> makeProcessActions(const Scenario* scenario);
/// With a scenario, a reader that reports no environment support: synthetic PIDs are not real
/// processes, and some may be the PIDs of real ones whose environment must not be shown for them.
[[nodiscard]] std::unique_ptr<Platform::IProcessEnvironmentReader> makeProcessEnvironmentReader(const Scenario* scenario);
/// With a scenario, a reader that reports no connections support, for the same reason (#799).
[[nodiscard]] std::unique_ptr<Platform::IProcessConnectionsReader> makeProcessConnectionsReader(const Scenario* scenario);
/// With a scenario, a reader that reports no modules support, for the same reason (#802).
[[nodiscard]] std::unique_ptr<Platform::IProcessModulesReader> makeProcessModulesReader(const Scenario* scenario);
/// With a scenario, a reader that reports no open-files support, for the same reason (#183).
[[nodiscard]] std::unique_ptr<Platform::IProcessOpenFilesReader> makeProcessOpenFilesReader(const Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::ISystemProbe> makeSystemProbe(const Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::IPowerProbe> makePowerProbe(const Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::IDiskProbe> makeDiskProbe(const Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::IGPUProbe> makeGPUProbe(const Scenario* scenario);

/// The history window (seconds) and refresh interval (ms) to start at: the scenario's when it sets
/// one, otherwise @p configured.
[[nodiscard]] int startupHistorySeconds(const Scenario* scenario, int configured) noexcept;
[[nodiscard]] int startupRefreshIntervalMs(const Scenario* scenario, int configured) noexcept;

// History preload. Each fills the model with historySamples() + 1 readings 100 ms apart
// (REFRESH_INTERVAL_MIN_MS), the last one interval before @p now, so the next live sample continues
// it; the first reading is the baseline the following deltas are taken from. The model's history
// window must already hold historySeconds. No-ops without a scenario or with history=none.

void preloadSystemHistory(const Scenario* scenario, Domain::SystemModel& model, std::chrono::steady_clock::time_point now);
void preloadStorageHistory(const Scenario* scenario, Domain::StorageModel& model, std::chrono::steady_clock::time_point now);
void preloadProcessHistory(const Scenario* scenario, Domain::ProcessModel& model, std::chrono::steady_clock::time_point now);

} // namespace App::Synthetic
