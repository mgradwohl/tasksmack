/// @file test_SyntheticScenario.cpp
/// @brief App::Synthetic (#1413): the TASKSMACK_SYNTHETIC parser, the composition helpers, and the
/// history preload filling the models to the full window -- and doing nothing at all when unset.

#include "App/SyntheticScenario.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Mocks/MockDiskProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/Synthetic/SyntheticProbes.h"
#include "Platform/Synthetic/SyntheticWorkload.h"
#include "Platform/SystemTypes.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{

namespace Sampling = Domain::Sampling;
using App::Synthetic::parseScenario;
using App::Synthetic::Scenario;
using App::Synthetic::ScenarioConfig;

/// Samples in the longest window at the fastest interval: 1800 s at 100 ms.
constexpr std::size_t FULL_HISTORY_SAMPLES =
    static_cast<std::size_t>(Sampling::HISTORY_SECONDS_MAX) * 1000U / static_cast<std::size_t>(Sampling::REFRESH_INTERVAL_MIN_MS);

[[nodiscard]] ScenarioConfig parsed(const char* value)
{
    const auto result = parseScenario(value);
    EXPECT_TRUE(result.config.has_value()) << value;
    return result.config.value_or(ScenarioConfig{});
}

/// A small machine (fast to build) with the given history.
[[nodiscard]] Scenario smallScenario(int historySeconds)
{
    ScenarioConfig config;
    config.workload = Platform::Synthetic::WorkloadSpec{.processes = 60, .cores = 4, .disks = 2, .interfaces = 3, .seed = 9};
    config.historySeconds = historySeconds;
    return Scenario(config);
}

} // namespace

// =============================================================================
// Parser
// =============================================================================

TEST(SyntheticScenarioParseTest, UnsetOrOffIsOff)
{
    for (const char* value : {static_cast<const char*>(nullptr), "", "   ", "0", "off", "OFF", "false", "no", " 0 "})
    {
        SCOPED_TRACE(value == nullptr ? "(null)" : value);
        const auto result = parseScenario(value);
        EXPECT_FALSE(result.config.has_value());
        EXPECT_TRUE(result.warnings.empty());
    }
}

TEST(SyntheticScenarioParseTest, FlagWordTakesTheDefaults)
{
    for (const char* value : {"1", "on", "true", "YES"})
    {
        SCOPED_TRACE(value);
        const auto result = parseScenario(value);
        ASSERT_TRUE(result.config.has_value());
        // value_or, not value(): clang-tidy's optional check does not model ASSERT_TRUE's early return.
        const ScenarioConfig resultConfig = result.config.value_or(ScenarioConfig{});
        EXPECT_TRUE(result.warnings.empty());
        const ScenarioConfig defaults;
        EXPECT_EQ(resultConfig.workload.processes, defaults.workload.processes);
        EXPECT_EQ(resultConfig.workload.cores, defaults.workload.cores);
        EXPECT_EQ(resultConfig.historySeconds, Sampling::HISTORY_SECONDS_MAX);
        EXPECT_FALSE(resultConfig.refreshIntervalMs.has_value());
    }
}

TEST(SyntheticScenarioParseTest, ReadsEveryKey)
{
    const auto result = parseScenario("processes=5000,history=full,cores=64,disks=8,interfaces=12,seed=7,refresh=250");
    ASSERT_TRUE(result.config.has_value());
    // value_or, not value(): clang-tidy's optional check does not model ASSERT_TRUE's early return.
    const ScenarioConfig resultConfig = result.config.value_or(ScenarioConfig{});
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(resultConfig.workload.processes, 5000U);
    EXPECT_EQ(resultConfig.workload.cores, 64U);
    EXPECT_EQ(resultConfig.workload.disks, 8U);
    EXPECT_EQ(resultConfig.workload.interfaces, 12U);
    EXPECT_EQ(resultConfig.workload.seed, 7U);
    EXPECT_EQ(resultConfig.historySeconds, Sampling::HISTORY_SECONDS_MAX);
    EXPECT_EQ(resultConfig.refreshIntervalMs, 250);
}

TEST(SyntheticScenarioParseTest, ToleratesSpacesCaseAndEmptyTokens)
{
    const ScenarioConfig config = parsed(" Processes = 10 ,, HISTORY=None , ");
    EXPECT_EQ(config.workload.processes, 10U);
    EXPECT_EQ(config.historySeconds, 0);
}

TEST(SyntheticScenarioParseTest, HistoryModes)
{
    struct Case
    {
        const char* value;
        int seconds;
        bool warns;
    };
    for (const Case& c : {Case{.value = "history=full", .seconds = Sampling::HISTORY_SECONDS_MAX, .warns = false},
                          Case{.value = "history=none", .seconds = 0, .warns = false},
                          Case{.value = "history=0", .seconds = 0, .warns = false},
                          Case{.value = "history=600", .seconds = 600, .warns = false},
                          Case{.value = "history=5", .seconds = Sampling::HISTORY_SECONDS_MIN, .warns = true},
                          Case{.value = "history=99999", .seconds = Sampling::HISTORY_SECONDS_MAX, .warns = true},
                          Case{.value = "history=lots", .seconds = Sampling::HISTORY_SECONDS_MAX, .warns = true}})
    {
        SCOPED_TRACE(c.value);
        const auto result = parseScenario(c.value);
        ASSERT_TRUE(result.config.has_value());
        // value_or, not value(): clang-tidy's optional check does not model ASSERT_TRUE's early return.
        const ScenarioConfig resultConfig = result.config.value_or(ScenarioConfig{});
        EXPECT_EQ(resultConfig.historySeconds, c.seconds);
        EXPECT_EQ(!result.warnings.empty(), c.warns);
    }
}

TEST(SyntheticScenarioParseTest, BadValuesWarnAndKeepTheScenarioOn)
{
    const ScenarioConfig defaults;
    struct Case
    {
        const char* value;
        std::size_t processes;
        std::size_t cores;
    };
    for (const Case& c :
         {Case{.value = "processes=abc", .processes = defaults.workload.processes, .cores = defaults.workload.cores},
          Case{.value = "processes=0", .processes = Platform::Synthetic::MIN_PROCESSES, .cores = defaults.workload.cores},
          Case{.value = "processes=-5", .processes = defaults.workload.processes, .cores = defaults.workload.cores},
          Case{.value = "processes=99999999999", .processes = Platform::Synthetic::MAX_PROCESSES, .cores = defaults.workload.cores},
          Case{.value = "cores=100000", .processes = defaults.workload.processes, .cores = Platform::Synthetic::MAX_CORES},
          Case{.value = "colour=blue", .processes = defaults.workload.processes, .cores = defaults.workload.cores},
          Case{.value = "bogus", .processes = defaults.workload.processes, .cores = defaults.workload.cores}})
    {
        SCOPED_TRACE(c.value);
        const auto result = parseScenario(c.value);
        ASSERT_TRUE(result.config.has_value());
        // value_or, not value(): clang-tidy's optional check does not model ASSERT_TRUE's early return.
        const ScenarioConfig resultConfig = result.config.value_or(ScenarioConfig{});
        EXPECT_FALSE(result.warnings.empty());
        EXPECT_EQ(resultConfig.workload.processes, c.processes);
        EXPECT_EQ(resultConfig.workload.cores, c.cores);
    }
}

TEST(SyntheticScenarioParseTest, RefreshIsClampedToTheSupportedRange)
{
    const auto fast = parseScenario("refresh=10");
    ASSERT_TRUE(fast.config.has_value());
    // value_or, not value(): clang-tidy's optional check does not model ASSERT_TRUE's early return.
    const ScenarioConfig fastConfig = fast.config.value_or(ScenarioConfig{});
    EXPECT_EQ(fastConfig.refreshIntervalMs, Sampling::REFRESH_INTERVAL_MIN_MS);
    EXPECT_FALSE(fast.warnings.empty());
    const auto bad = parseScenario("refresh=soon");
    ASSERT_TRUE(bad.config.has_value());
    // value_or, not value(): clang-tidy's optional check does not model ASSERT_TRUE's early return.
    const ScenarioConfig badConfig = bad.config.value_or(ScenarioConfig{});
    EXPECT_FALSE(badConfig.refreshIntervalMs.has_value());
    EXPECT_FALSE(bad.warnings.empty());
}

TEST(SyntheticScenarioParseTest, DescribeNamesTheSettings)
{
    const std::string text = App::Synthetic::describe(parsed("processes=123,refresh=500"));
    EXPECT_NE(text.find("processes=123"), std::string::npos);
    EXPECT_NE(text.find("refresh=500ms"), std::string::npos);
    EXPECT_NE(text.find(std::to_string(Sampling::HISTORY_SECONDS_MAX)), std::string::npos);
}

TEST(SyntheticScenarioTest, HistorySamplesFollowSamplingConfig)
{
    EXPECT_EQ(smallScenario(Sampling::HISTORY_SECONDS_MAX).historySamples(), FULL_HISTORY_SAMPLES);
    EXPECT_EQ(FULL_HISTORY_SAMPLES, 18000U); // the documented headline figure
    EXPECT_EQ(smallScenario(60).historySamples(), 600U);
    EXPECT_EQ(smallScenario(0).historySamples(), 0U);
}

TEST(SyntheticScenarioTest, ActiveScenarioFollowsTheEnvironment)
{
    // CI leaves TASKSMACK_SYNTHETIC unset; either way the active scenario is what the parser makes of it.
    const std::string name(App::Synthetic::ENV_VAR);
    const bool on = parseScenario(SDL_getenv(name.c_str())).config.has_value();
    EXPECT_EQ(App::Synthetic::activeScenario() != nullptr, on);
    EXPECT_EQ(App::Synthetic::activeScenario(), App::Synthetic::activeScenario()); // read once
}

// =============================================================================
// Unset: no effect
// =============================================================================

TEST(SyntheticScenarioTest, WithoutAScenarioProbesAreThePlatformsOwn)
{
    const auto processProbe = App::Synthetic::makeProcessProbe(nullptr);
    ASSERT_NE(processProbe, nullptr);
    EXPECT_EQ(dynamic_cast<Platform::Synthetic::SyntheticProcessProbe*>(processProbe.get()), nullptr);
    const auto systemProbe = App::Synthetic::makeSystemProbe(nullptr);
    ASSERT_NE(systemProbe, nullptr);
    EXPECT_EQ(dynamic_cast<Platform::Synthetic::SyntheticSystemProbe*>(systemProbe.get()), nullptr);
    const auto diskProbe = App::Synthetic::makeDiskProbe(nullptr);
    ASSERT_NE(diskProbe, nullptr);
    EXPECT_EQ(dynamic_cast<Platform::Synthetic::SyntheticDiskProbe*>(diskProbe.get()), nullptr);
    const auto actions = App::Synthetic::makeProcessActions(nullptr);
    ASSERT_NE(actions, nullptr);
    EXPECT_EQ(dynamic_cast<Platform::Synthetic::SyntheticProcessActions*>(actions.get()), nullptr);

    EXPECT_EQ(App::Synthetic::startupHistorySeconds(nullptr, 300), 300);
    EXPECT_EQ(App::Synthetic::startupRefreshIntervalMs(nullptr, 1000), 1000);
}

TEST(SyntheticScenarioTest, WithoutAScenarioPreloadDoesNothing)
{
    Domain::SystemModel system(std::make_unique<TestMocks::MockSystemProbe>());
    Domain::StorageModel storage(std::make_unique<Mocks::MockDiskProbe>());
    Domain::ProcessModel processes(std::make_unique<TestMocks::MockProcessProbe>());
    const auto now = std::chrono::steady_clock::now();

    App::Synthetic::preloadSystemHistory(nullptr, system, now);
    App::Synthetic::preloadStorageHistory(nullptr, storage, now);
    App::Synthetic::preloadProcessHistory(nullptr, processes, now);

    // history=none is a scenario without a preload.
    const Scenario noHistory = smallScenario(0);
    App::Synthetic::preloadSystemHistory(&noHistory, system, now);
    App::Synthetic::preloadStorageHistory(&noHistory, storage, now);
    App::Synthetic::preloadProcessHistory(&noHistory, processes, now);

    EXPECT_EQ(system.publicationVersion(), 0U);
    EXPECT_TRUE(system.timestamps().empty());
    EXPECT_EQ(storage.publicationVersion(), 0U);
    EXPECT_TRUE(storage.historyTimestamps().empty());
    EXPECT_TRUE(processes.historyTimestamps().empty());
    Domain::ProcessSystemHistories histories;
    EXPECT_FALSE(processes.tryCopySystemHistoriesIfNewer(0, histories));
}

// =============================================================================
// With a scenario
// =============================================================================

TEST(SyntheticScenarioTest, ScenarioProbesAreSynthetic)
{
    const Scenario scenario = smallScenario(0);
    EXPECT_NE(dynamic_cast<Platform::Synthetic::SyntheticProcessProbe*>(App::Synthetic::makeProcessProbe(&scenario).get()), nullptr);
    EXPECT_NE(dynamic_cast<Platform::Synthetic::SyntheticSystemProbe*>(App::Synthetic::makeSystemProbe(&scenario).get()), nullptr);
    EXPECT_NE(dynamic_cast<Platform::Synthetic::SyntheticDiskProbe*>(App::Synthetic::makeDiskProbe(&scenario).get()), nullptr);
    EXPECT_NE(dynamic_cast<Platform::Synthetic::SyntheticProcessActions*>(App::Synthetic::makeProcessActions(&scenario).get()), nullptr);
    // Nothing real is mixed in: no power or GPU probe.
    EXPECT_EQ(App::Synthetic::makePowerProbe(&scenario), nullptr);
    EXPECT_EQ(App::Synthetic::makeGPUProbe(&scenario), nullptr);
}

TEST(SyntheticScenarioTest, StartupOverridesComeFromTheScenario)
{
    const auto config = parsed("history=600,refresh=100");
    const Scenario scenario(config);
    EXPECT_EQ(App::Synthetic::startupHistorySeconds(&scenario, 300), 600);
    EXPECT_EQ(App::Synthetic::startupRefreshIntervalMs(&scenario, 1000), Sampling::REFRESH_INTERVAL_MIN_MS);
    const Scenario noOverrides = smallScenario(0);
    EXPECT_EQ(App::Synthetic::startupHistorySeconds(&noOverrides, 300), 300);
    EXPECT_EQ(App::Synthetic::startupRefreshIntervalMs(&noOverrides, 1000), 1000);
}

TEST(SyntheticScenarioTest, PreloadFillsTheFullWindow)
{
    const Scenario scenario = smallScenario(Sampling::HISTORY_SECONDS_MAX);
    const std::size_t samples = scenario.historySamples();
    ASSERT_EQ(samples, FULL_HISTORY_SAMPLES);
    const auto now = std::chrono::steady_clock::now();
    const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const double interval = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 1000.0;

    Domain::SystemModel system(App::Synthetic::makeSystemProbe(&scenario));
    system.setMaxHistorySeconds(scenario.config().historySeconds);
    App::Synthetic::preloadSystemHistory(&scenario, system, now);
    {
        SCOPED_TRACE("SystemModel");
        const auto publication = system.publication();
        EXPECT_EQ(system.publicationVersion(), 1U); // one publish for the whole preload
        ASSERT_EQ(publication->timestamps.size(), samples);
        EXPECT_EQ(publication->cpuHistory.size(), samples);
        ASSERT_EQ(publication->perCoreHistory.size(), 4U);
        EXPECT_EQ(publication->perCoreHistory.front().size(), samples);
        EXPECT_EQ(publication->perInterfaceRxHistory.size(), 3U);
        EXPECT_NEAR(publication->timestamps.back(), nowSeconds - interval, 1e-6);
        EXPECT_NEAR(
            publication->timestamps.back() - publication->timestamps.front(), (static_cast<double>(samples) - 1.0) * interval, 1e-6);
        EXPECT_GT(publication->cpuHistory.back(), 0.0F);
        EXPECT_LT(publication->cpuHistory.back(), 100.0F);
    }

    Domain::StorageModel storage(App::Synthetic::makeDiskProbe(&scenario));
    storage.setMaxHistorySeconds(scenario.config().historySeconds);
    App::Synthetic::preloadStorageHistory(&scenario, storage, now);
    {
        SCOPED_TRACE("StorageModel");
        const auto publication = storage.publication();
        EXPECT_EQ(storage.publicationVersion(), 1U);
        // Every reading is a sample; the baseline's has no rate yet.
        ASSERT_EQ(publication->timestamps.size(), samples + 1);
        ASSERT_EQ(publication->perDiskHistory.size(), 2U);
        EXPECT_EQ(publication->perDiskHistory.front().readBytesPerSec.size(), samples + 1);
        EXPECT_GT(publication->totalReadHistory.back(), 0.0);
    }

    Domain::ProcessModel processes(App::Synthetic::makeProcessProbe(&scenario));
    processes.setMaxHistorySeconds(scenario.config().historySeconds);
    App::Synthetic::preloadProcessHistory(&scenario, processes, now);
    {
        SCOPED_TRACE("ProcessModel");
        Domain::ProcessSystemHistories histories;
        ASSERT_TRUE(processes.tryCopySystemHistoriesIfNewer(0, histories));
        EXPECT_EQ(histories.version, 1U);
        ASSERT_EQ(histories.timestamps.size(), samples + 1);
        ASSERT_EQ(histories.threadCount.size(), samples + 1);
        EXPECT_DOUBLE_EQ(histories.threadCount.back(),
                         scenario.workload()->processTotalsAt(scenario.workload()->uptimeAt(now)).threadCount);
        EXPECT_GT(histories.power.back(), 0.0);
    }

    // Live sampling continues the preloaded history and stays within the window's ring.
    // A live sample at the preload's "now" continues it, and the window stays full.
    Platform::SystemCounters liveCounters;
    scenario.workload()->systemCountersAt(scenario.workload()->uptimeAt(now), liveCounters);
    system.updateFromCounters(liveCounters, nowSeconds);
    const auto liveTimestamps = system.timestamps();
    EXPECT_GE(liveTimestamps.size(), samples);
    EXPECT_LE(liveTimestamps.size(), Sampling::historyCapacityForSeconds(Sampling::HISTORY_SECONDS_MAX));
    EXPECT_GE(liveTimestamps.back() - liveTimestamps.front(), static_cast<double>(Sampling::HISTORY_SECONDS_MAX) - (2.0 * interval));
    storage.sample();
    EXPECT_LE(storage.historyTimestamps().size(), Sampling::historyCapacityForSeconds(Sampling::HISTORY_SECONDS_MAX));
    processes.refresh();
    EXPECT_EQ(processes.processCount(), 60U);
}

TEST(SyntheticScenarioTest, PreloadIsTrimmedToAShorterWindow)
{
    // A model whose window is shorter than the scenario's history keeps only its window.
    const Scenario scenario = smallScenario(600);
    Domain::SystemModel system(App::Synthetic::makeSystemProbe(&scenario));
    system.setMaxHistorySeconds(60);
    App::Synthetic::preloadSystemHistory(&scenario, system, std::chrono::steady_clock::now());
    const auto timestamps = system.timestamps();
    ASSERT_FALSE(timestamps.empty());
    EXPECT_LE(timestamps.back() - timestamps.front(), 60.0 + 0.2);
    EXPECT_LE(timestamps.size(), Sampling::historyCapacityForSeconds(60));
}
