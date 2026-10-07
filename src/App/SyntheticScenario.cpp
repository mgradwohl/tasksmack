#include "SyntheticScenario.h"

#include "Core/EnvUtils.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/StorageTypes.h"
#include "Platform/Synthetic/SyntheticProbes.h"
#include "Platform/Synthetic/SyntheticWorkload.h"
#include "Platform/SystemTypes.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace App::Synthetic
{

namespace
{

namespace Sampling = Domain::Sampling;

/// The spacing of preloaded samples: the fastest refresh interval.
constexpr std::chrono::milliseconds PRELOAD_INTERVAL{Sampling::REFRESH_INTERVAL_MIN_MS};

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    const auto isSpace = [](char c)
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

[[nodiscard]] std::string lower(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> parseUnsigned(std::string_view text) noexcept
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
    {
        return std::nullopt;
    }
    return value;
}

/// Parses a count into @p out, clamped to [lo, hi], with a warning for a bad or clamped value.
void parseCount(
    std::string_view key, std::string_view text, std::size_t lo, std::size_t hi, std::size_t& out, std::vector<std::string>& warnings)
{
    const auto value = parseUnsigned(text);
    if (!value)
    {
        warnings.push_back(std::format("{}={}: not a whole number; using {}", key, text, out));
        return;
    }
    const std::uint64_t clamped = std::clamp<std::uint64_t>(*value, lo, hi);
    if (clamped != *value)
    {
        warnings.push_back(std::format("{}={}: out of range; using {}", key, text, clamped));
    }
    out = static_cast<std::size_t>(clamped);
}

/// The time of preload reading @p index of @p readings: the last is one interval before @p now.
[[nodiscard]] std::chrono::steady_clock::time_point
readingTime(std::chrono::steady_clock::time_point now, std::size_t readings, std::size_t index)
{
    const auto intervalsBack = static_cast<std::chrono::milliseconds::rep>(readings - index);
    return now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(PRELOAD_INTERVAL * intervalsBack);
}

[[nodiscard]] double steadySeconds(std::chrono::steady_clock::time_point time) noexcept
{
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

/// Readings a preload takes: one per interval plus the baseline. 0 when there is nothing to preload.
[[nodiscard]] std::size_t preloadReadings(const Scenario* scenario) noexcept
{
    if (scenario == nullptr || scenario->historySamples() == 0)
    {
        return 0;
    }
    return scenario->historySamples() + 1;
}

} // namespace

ParseResult parseScenario(const char* value)
{
    ParseResult result;
    if (!Core::isEnvFlagEnabled(value))
    {
        return result;
    }
    const std::string_view text = trim(value);
    if (text.empty() || !Core::isEnvFlagEnabled(std::string(text).c_str()))
    {
        return result;
    }

    ScenarioConfig config;
    config.historySeconds = Sampling::HISTORY_SECONDS_MAX;
    std::string_view rest = text;
    while (!rest.empty())
    {
        const std::size_t comma = rest.find(',');
        const std::string_view token = trim(rest.substr(0, comma));
        rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
        if (token.empty())
        {
            continue;
        }
        const std::size_t equals = token.find('=');
        if (equals == std::string_view::npos)
        {
            // A bare flag word ("1", "on") just turns the scenario on.
            if (const std::string word = lower(token); word != "1" && word != "on" && word != "true" && word != "yes")
            {
                result.warnings.push_back(std::format("'{}': not a key=value setting; ignored", token));
            }
            continue;
        }
        const std::string key = lower(trim(token.substr(0, equals)));
        const std::string_view setting = trim(token.substr(equals + 1));
        if (key == "processes")
        {
            parseCount(key,
                       setting,
                       Platform::Synthetic::MIN_PROCESSES,
                       Platform::Synthetic::MAX_PROCESSES,
                       config.workload.processes,
                       result.warnings);
        }
        else if (key == "cores")
        {
            parseCount(
                key, setting, Platform::Synthetic::MIN_CORES, Platform::Synthetic::MAX_CORES, config.workload.cores, result.warnings);
        }
        else if (key == "disks")
        {
            parseCount(key, setting, 0, Platform::Synthetic::MAX_DISKS, config.workload.disks, result.warnings);
        }
        else if (key == "interfaces")
        {
            parseCount(key, setting, 0, Platform::Synthetic::MAX_INTERFACES, config.workload.interfaces, result.warnings);
        }
        else if (key == "seed")
        {
            if (const auto seed = parseUnsigned(setting))
            {
                config.workload.seed = *seed;
            }
            else
            {
                result.warnings.push_back(std::format("seed={}: not a whole number; using {}", setting, config.workload.seed));
            }
        }
        else if (key == "history")
        {
            const std::string mode = lower(setting);
            if (mode == "full")
            {
                config.historySeconds = Sampling::HISTORY_SECONDS_MAX;
            }
            else if (mode == "none" || mode == "0")
            {
                config.historySeconds = 0;
            }
            else if (const auto seconds = parseUnsigned(setting))
            {
                const auto requested = static_cast<int>(std::min<std::uint64_t>(*seconds, std::numeric_limits<int>::max()));
                config.historySeconds = Sampling::clampHistorySeconds(requested);
                if (std::cmp_not_equal(config.historySeconds, *seconds))
                {
                    result.warnings.push_back(std::format("history={}: out of range; using {}", setting, config.historySeconds));
                }
            }
            else
            {
                result.warnings.push_back(
                    std::format("history={}: expected full, none or seconds; using {}", setting, config.historySeconds));
            }
        }
        else if (key == "refresh")
        {
            if (const auto ms = parseUnsigned(setting))
            {
                const auto requested = static_cast<int>(std::min<std::uint64_t>(*ms, std::numeric_limits<int>::max()));
                config.refreshIntervalMs = Sampling::clampRefreshInterval(requested);
                if (std::cmp_not_equal(*config.refreshIntervalMs, *ms))
                {
                    result.warnings.push_back(std::format("refresh={}: out of range; using {}", setting, *config.refreshIntervalMs));
                }
            }
            else
            {
                result.warnings.push_back(std::format("refresh={}: not a whole number of milliseconds; ignored", setting));
            }
        }
        else
        {
            result.warnings.push_back(std::format("unknown key '{}'; ignored", key));
        }
    }
    result.config = config;
    return result;
}

std::string describe(const ScenarioConfig& config)
{
    return std::format("processes={} cores={} disks={} interfaces={} seed={} history={}s refresh={}",
                       config.workload.processes,
                       config.workload.cores,
                       config.workload.disks,
                       config.workload.interfaces,
                       config.workload.seed,
                       config.historySeconds,
                       config.refreshIntervalMs ? std::format("{}ms", *config.refreshIntervalMs) : std::string("configured"));
}

Scenario::Scenario(const ScenarioConfig& config) : Scenario(config, std::make_shared<const Platform::Synthetic::Workload>(config.workload))
{}

Scenario::Scenario(const ScenarioConfig& config, std::shared_ptr<const Platform::Synthetic::Workload> workload)
    : m_Config(config), m_Workload(std::move(workload))
{}

std::size_t Scenario::historySamples() const noexcept
{
    if (m_Config.historySeconds <= 0)
    {
        return 0;
    }
    return static_cast<std::size_t>(m_Config.historySeconds) * 1000U / static_cast<std::size_t>(Sampling::REFRESH_INTERVAL_MIN_MS);
}

const Scenario* activeScenario()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<Scenario> scenario = []() -> std::optional<Scenario>
    {
        const std::string name(ENV_VAR);
        ParseResult parsed = parseScenario(SDL_getenv(name.c_str()));
        for (const auto& warning : parsed.warnings)
        {
            spdlog::warn("{}: {}", ENV_VAR, warning);
        }
        if (!parsed.config)
        {
            return std::nullopt;
        }
        // warn, not info, so even a release build (default level warn) says the data is not this machine's.
        spdlog::warn(
            "{} is set: showing a synthetic machine, not this one ({}); process actions are disabled", ENV_VAR, describe(*parsed.config));
        return std::optional<Scenario>(std::in_place, *parsed.config);
    }();
    return scenario ? &*scenario : nullptr;
}

std::unique_ptr<Platform::IProcessProbe> makeProcessProbe(const Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_unique<Platform::Synthetic::SyntheticProcessProbe>(scenario->workload());
    }
    return Platform::makeProcessProbe();
}

std::unique_ptr<Platform::IProcessActions> makeProcessActions(const Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_unique<Platform::Synthetic::SyntheticProcessActions>();
    }
    return Platform::makeProcessActions();
}

std::unique_ptr<Platform::ISystemProbe> makeSystemProbe(const Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_unique<Platform::Synthetic::SyntheticSystemProbe>(scenario->workload());
    }
    return Platform::makeSystemProbe();
}

std::unique_ptr<Platform::IPowerProbe> makePowerProbe(const Scenario* scenario)
{
    return (scenario != nullptr) ? nullptr : Platform::makePowerProbe();
}

std::unique_ptr<Platform::IDiskProbe> makeDiskProbe(const Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_unique<Platform::Synthetic::SyntheticDiskProbe>(scenario->workload());
    }
    return Platform::makeDiskProbe();
}

std::unique_ptr<Platform::IGPUProbe> makeGPUProbe(const Scenario* scenario)
{
    return (scenario != nullptr) ? nullptr : Platform::makeGPUProbe();
}

int startupHistorySeconds(const Scenario* scenario, int configured) noexcept
{
    return (scenario != nullptr && scenario->config().historySeconds > 0) ? scenario->config().historySeconds : configured;
}

int startupRefreshIntervalMs(const Scenario* scenario, int configured) noexcept
{
    return (scenario != nullptr && scenario->config().refreshIntervalMs) ? *scenario->config().refreshIntervalMs : configured;
}

void preloadSystemHistory(const Scenario* scenario, Domain::SystemModel& model, std::chrono::steady_clock::time_point now)
{
    const std::size_t readings = preloadReadings(scenario);
    if (readings == 0)
    {
        return;
    }
    const Platform::Synthetic::Workload& workload = *scenario->workload();
    std::size_t index = 0;
    model.updateFromCounterSeries(
        [&](Platform::SystemCounters& counters, double& nowSeconds)
        {
            if (index >= readings)
            {
                return false;
            }
            const auto when = readingTime(now, readings, index++);
            workload.systemCountersAt(workload.uptimeAt(when), counters);
            nowSeconds = steadySeconds(when);
            return true;
        });
}

void preloadStorageHistory(const Scenario* scenario, Domain::StorageModel& model, std::chrono::steady_clock::time_point now)
{
    const std::size_t readings = preloadReadings(scenario);
    if (readings == 0)
    {
        return;
    }
    const Platform::Synthetic::Workload& workload = *scenario->workload();
    std::size_t index = 0;
    model.sampleSeries(
        [&](Platform::SystemDiskCounters& counters, std::chrono::steady_clock::time_point& when)
        {
            if (index >= readings)
            {
                return false;
            }
            when = readingTime(now, readings, index++);
            workload.diskCountersAt(workload.uptimeAt(when), counters);
            return true;
        });
}

void preloadProcessHistory(const Scenario* scenario, Domain::ProcessModel& model, std::chrono::steady_clock::time_point now)
{
    const std::size_t readings = preloadReadings(scenario);
    if (readings == 0)
    {
        return;
    }
    const Platform::Synthetic::Workload& workload = *scenario->workload();
    std::size_t index = 0;
    model.appendSystemHistory(
        [&](Domain::ProcessSystemHistorySample& sample)
        {
            if (index >= readings)
            {
                return false;
            }
            const auto when = readingTime(now, readings, index++);
            const Platform::Synthetic::ProcessTotals totals = workload.processTotalsAt(workload.uptimeAt(when));
            sample = Domain::ProcessSystemHistorySample{.timeSeconds = steadySeconds(when),
                                                        .netSentBytesPerSec = totals.netSentBytesPerSec,
                                                        .netReceivedBytesPerSec = totals.netReceivedBytesPerSec,
                                                        .pageFaultsPerSec = totals.pageFaultsPerSec,
                                                        .threadCount = totals.threadCount,
                                                        .handleCount = totals.handleCount,
                                                        .powerWatts = totals.powerWatts};
            return true;
        });
}

} // namespace App::Synthetic
