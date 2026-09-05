// Windows-specific benchmark for Platform::PDHGPUProbe, added while investigating
// perf-plan-574 / issue #583: instanceFor() (the per-instance-name cache lookup inside
// readProcessGPUCounters()) measured as ~61% of TaskSmack's own CPU time in a GPU-heavy
// ETW benchmark trace, and 8.4% even in a 45s idle app trace. This benchmark reports the
// instance-cache's hit/miss/clear counts alongside timing, to distinguish "high call volume
// against a warm cache" from "cache churn forcing repeated parses" as the actual cause.

#include "Platform/Windows/PDHGPUProbe.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

// ============================================================================
// Synthetic instanceFor() cache microbenchmark
//
// Answers: how much of instanceFor()'s cost is hashing the full ~40-70 character PDH
// instance name (e.g. "pid_12345_luid_0x00000000_0x0000d3a0_phys_0_eng_12_engtype_
// VideoProcessing"), versus hashing just the pid/luid/phys/eng prefix that alone
// identifies the instance -- the trailing "_engtype_<TYPE>" suffix is redundant for
// uniqueness (a given pid+luid+phys+eng combination has exactly one engine type). This is
// synthetic (no real PDH/GPU hardware needed) so it's repeatable in CI, and isolates the
// hashing/lookup cost from everything else readProcessGPUCounters() does.
// ============================================================================

// Mirrors PDHGPUProbe::Impl::WideStringHash exactly (that one is private to the Impl
// struct, so not reusable directly from here).
struct WideStringHash
{
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::wstring_view value) const noexcept
    {
        return std::hash<std::wstring_view>{}(value);
    }
};

std::wstring toHex4(unsigned value)
{
    static constexpr std::array<wchar_t, 16> digits = {
        L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7', L'8', L'9', L'a', L'b', L'c', L'd', L'e', L'f'};
    std::wstring result(4, L'0');
    for (int i = 3; i >= 0; --i)
    {
        result[static_cast<std::size_t>(i)] = digits.at(value & 0xFU);
        value >>= 4U;
    }
    return result;
}

// Builds a realistic mix of "GPU Engine" (per-pid-per-engine, with an "_engtype_<TYPE>"
// suffix) and "GPU Process Memory" (per-pid only, no engine/type suffix) instance names,
// at roughly the same ~660-instance scale observed on real hardware (see the code comment
// in PDHGPUProbeImpl.h referencing "a 651-instance system").
std::vector<std::wstring> makeSyntheticInstanceNames(std::size_t engineInstanceCount, std::size_t memoryInstanceCount)
{
    static constexpr std::array<const wchar_t*, 6> engineTypes = {
        L"3D", L"Copy", L"Compute", L"VideoEncode", L"VideoDecode", L"VideoProcessing"};

    std::vector<std::wstring> names;
    names.reserve(engineInstanceCount + memoryInstanceCount);

    for (std::size_t i = 0; i < engineInstanceCount; ++i)
    {
        const unsigned pid = 1000U + static_cast<unsigned>(i % 40);
        const auto eng = static_cast<unsigned>(i % 16);
        const wchar_t* type = engineTypes.at(i % engineTypes.size());
        names.push_back(L"pid_" + std::to_wstring(pid) + L"_luid_0x00000000_0x0000" + toHex4(static_cast<unsigned>(i)) + L"_phys_0_eng_" +
                        std::to_wstring(eng) + L"_engtype_" + type);
    }
    for (std::size_t i = 0; i < memoryInstanceCount; ++i)
    {
        const unsigned pid = 1000U + static_cast<unsigned>(i % 40);
        names.push_back(L"pid_" + std::to_wstring(pid) + L"_luid_0x00000000_0x0000" + toHex4(static_cast<unsigned>(i)) + L"_phys_0");
    }
    return names;
}

// Everything before "_engtype_", or the whole name unchanged if there's no such suffix
// (already the case for "GPU Process Memory" instances).
std::wstring_view trimmedKey(std::wstring_view fullName)
{
    constexpr std::wstring_view engtypeMarker = L"_engtype_";
    const auto pos = fullName.find(engtypeMarker);
    return (pos == std::wstring_view::npos) ? fullName : fullName.substr(0, pos);
}

static void BM_PDHInstanceCache_FullKeyLookup(benchmark::State& state)
{
    const auto names = makeSyntheticInstanceNames(600, 60);
    std::unordered_map<std::wstring, int, WideStringHash, std::equal_to<>> cache;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        cache.emplace(names[i], static_cast<int>(i));
    }

    for (auto _ : state)
    {
        for (const auto& name : names)
        {
            const auto it = cache.find(std::wstring_view(name));
            benchmark::DoNotOptimize(it);
        }
    }
    state.counters["lookups_per_iter"] = benchmark::Counter(static_cast<double>(names.size()));
}
BENCHMARK(BM_PDHInstanceCache_FullKeyLookup);

static void BM_PDHInstanceCache_TrimmedKeyLookup(benchmark::State& state)
{
    const auto names = makeSyntheticInstanceNames(600, 60);
    std::unordered_map<std::wstring, int, WideStringHash, std::equal_to<>> cache;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        cache.emplace(std::wstring(trimmedKey(names[i])), static_cast<int>(i));
    }

    for (auto _ : state)
    {
        for (const auto& name : names)
        {
            // trimmedKey() itself must scan for "_engtype_" on every call, same as real
            // production code would need to - this measures the net effect, not just the
            // hash cost in isolation.
            const auto it = cache.find(trimmedKey(std::wstring_view(name)));
            benchmark::DoNotOptimize(it);
        }
    }
    state.counters["lookups_per_iter"] = benchmark::Counter(static_cast<double>(names.size()));
}
BENCHMARK(BM_PDHInstanceCache_TrimmedKeyLookup);

static void BM_PDHGPUProbe_ReadProcessGPUCounters(benchmark::State& state)
{
    Platform::PDHGPUProbe probe;

    if (!probe.isAvailable())
    {
        state.SkipWithMessage("PDH GPU counters not available on this machine");
        return;
    }

    // First call is a warm-up sample (PDH needs two collects to compute deltas) and primes
    // the instance cache; excluded from the timed loop below.
    auto warmup = probe.readProcessGPUCounters();
    benchmark::DoNotOptimize(warmup);

    for (auto _ : state)
    {
        auto counters = probe.readProcessGPUCounters();
        benchmark::DoNotOptimize(counters.data());
        benchmark::DoNotOptimize(counters.size());
    }

    const auto stats = probe.instanceCacheStats();
    const auto totalLookups = stats.hits + stats.misses;
    const auto totalPositional = stats.positionalHits + stats.positionalMisses;
    state.counters["cache_hits"] = benchmark::Counter(static_cast<double>(stats.hits));
    state.counters["cache_misses"] = benchmark::Counter(static_cast<double>(stats.misses));
    state.counters["cache_clears"] = benchmark::Counter(static_cast<double>(stats.clears));
    state.counters["cached_entries"] = benchmark::Counter(static_cast<double>(stats.cachedEntries));
    state.counters["hit_rate_pct"] =
        benchmark::Counter(totalLookups > 0 ? (100.0 * static_cast<double>(stats.hits) / static_cast<double>(totalLookups)) : 0.0);
    state.counters["positional_hits"] = benchmark::Counter(static_cast<double>(stats.positionalHits));
    state.counters["positional_misses"] = benchmark::Counter(static_cast<double>(stats.positionalMisses));
    state.counters["positional_hit_rate_pct"] = benchmark::Counter(
        totalPositional > 0 ? (100.0 * static_cast<double>(stats.positionalHits) / static_cast<double>(totalPositional)) : 0.0);

    auto counters = probe.readProcessGPUCounters();
    state.counters["gpu_process_entries"] = benchmark::Counter(static_cast<double>(counters.size()));
}
BENCHMARK(BM_PDHGPUProbe_ReadProcessGPUCounters);

} // namespace
