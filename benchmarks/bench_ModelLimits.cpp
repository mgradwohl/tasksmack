// Domain model benchmarks at the limits: full history, high cardinality, and a UI reader racing a
// publishing writer (#1422).
//
// The other model benchmarks run real probes against short, startup-sized histories. These feed the
// models synthetic counters from tests/Mocks probes instead -- deterministic, and the same on every
// machine -- and hold them at a steady history length, so each iteration measures one sample at the
// size that matters:
//
//   FullHistory  -- one sample (append, trim, publish) with N samples retained, N = 300 / 3k / 18k.
//                   18k is the longest history: 1800 s (HISTORY_SECONDS_MAX) at 100 ms
//                   (REFRESH_INTERVAL_MIN_MS). Until #1412 publish() copied every series into each publication.
//   Cardinality  -- the same at a 300-sample history with many cores, interfaces or disks, and one
//                   ProcessModel refresh with many processes. CardinalityHistory: 64 cores and 10
//                   interfaces, or 16 disks, at 3k and 18k samples.
//   Concurrent   -- how long a UI-style System/Storage/GPUModel::publication() call waits when it lands on
//                   a publish in another thread. Before #868 publish() ran under the model's exclusive
//                   lock, so this was the lock hold a frame could be stuck behind; now it is a swap.
//   GPUModel     -- History_Publish and Concurrent on a mock GPU probe, at a 3k-sample history. Being
//                   mocks, they run on a GPU-less runner too, unlike bench_GPUModel.cpp's (#1420).
//
// History is held at N samples by setting the model's window to N sample intervals: trimming then
// drops one old sample per new one. Each configuration's model is built once per process and reused
// by every repetition (cachedFixture()); a further sample leaves it at the same size.

#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "Mocks/MockDiskProbe.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

namespace Sampling = Domain::Sampling;

/// The fastest refresh interval (REFRESH_INTERVAL_MIN_MS), the spacing of every synthetic sample.
constexpr double SAMPLE_INTERVAL_SECONDS = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 1000.0;

/// Samples a @p historySeconds window holds at a @p refreshMs cadence.
[[nodiscard]] constexpr std::int64_t samplesFor(int historySeconds, int refreshMs) noexcept
{
    return static_cast<std::int64_t>(historySeconds) * 1000 / refreshMs;
}

/// The history lengths the benchmarks hold, all from SamplingConfig.h (and so their names, e.g.
/// .../18000, follow it):
/// - the default window at the default refresh: 5 minutes at 1 s (300)
/// - the default window at the fastest refresh: 5 minutes at 100 ms (3000)
/// - the longest window at the fastest refresh: 30 minutes at 100 ms (18000), the headline figure
constexpr std::int64_t DEFAULT_HISTORY_SAMPLES = samplesFor(Sampling::HISTORY_SECONDS_DEFAULT, Sampling::REFRESH_INTERVAL_DEFAULT_MS);
constexpr std::int64_t DEFAULT_WINDOW_FAST_SAMPLES = samplesFor(Sampling::HISTORY_SECONDS_DEFAULT, Sampling::REFRESH_INTERVAL_MIN_MS);
constexpr std::int64_t FULL_HISTORY_SAMPLES = samplesFor(Sampling::HISTORY_SECONDS_MAX, Sampling::REFRESH_INTERVAL_MIN_MS);
/// Sample times start here (seconds on the models' steady_clock-epoch time base), clear of zero.
constexpr double START_SECONDS = 1000.0;

/// The window that holds @p samples samples at SAMPLE_INTERVAL_SECONDS.
[[nodiscard]] double windowSecondsFor(std::int64_t samples)
{
    return static_cast<double>(samples) * SAMPLE_INTERVAL_SECONDS;
}

/// One fixture per configuration for the life of the process: see the file comment.
template<typename Fixture, typename Key, typename Make> Fixture& cachedFixture(const Key& key, Make&& make)
{
    static std::map<Key, std::unique_ptr<Fixture>> fixtures;
    auto& slot = fixtures[key];
    if (!slot)
    {
        slot = std::forward<Make>(make)();
    }
    return *slot;
}

// =============================================================================
// SystemModel
// =============================================================================

/// SystemCounters for sample @p step: every counter advances, so each sample has fresh deltas, with a
/// load that varies by core and over time.
class SystemCounterFeed
{
  public:
    SystemCounterFeed(std::size_t cores, std::size_t interfaces)
    {
        std::vector<Platform::CpuCounters> perCore(cores);
        std::vector<Platform::SystemCounters::InterfaceCounters> ifaces;
        ifaces.reserve(interfaces);
        for (std::size_t i = 0; i < interfaces; ++i)
        {
            ifaces.push_back(TestMocks::makeInterfaceCounters("eth" + std::to_string(i)));
        }
        m_Counters = TestMocks::makeSystemCounters(
            TestMocks::makeCpuCounters(0, 0, 0, 0), TestMocks::makeMemoryAtUsage(50.0), 0, std::move(perCore), 0, 0, std::move(ifaces));
    }

    [[nodiscard]] const Platform::SystemCounters& at(std::uint64_t step)
    {
        const auto advance = [step](Platform::CpuCounters& cpu, std::uint64_t salt)
        {
            const std::uint64_t busy = 2 + ((step + salt) % 7);
            cpu.user += busy;
            cpu.system += 1 + ((step + salt) % 3);
            cpu.iowait += (step + salt) % 2;
            cpu.idle += 10 - busy;
        };
        advance(m_Counters.cpuTotal, 0);
        for (std::size_t core = 0; core < m_Counters.cpuPerCore.size(); ++core)
        {
            advance(m_Counters.cpuPerCore[core], core);
        }
        std::uint64_t rx = 0;
        std::uint64_t tx = 0;
        for (std::size_t i = 0; i < m_Counters.networkInterfaces.size(); ++i)
        {
            auto& iface = m_Counters.networkInterfaces[i];
            iface.rxBytes += 1000 + (((step + i) % 13) * 100);
            iface.txBytes += 500 + (((step + i) % 11) * 50);
            rx += iface.rxBytes;
            tx += iface.txBytes;
        }
        m_Counters.netRxBytes = rx;
        m_Counters.netTxBytes = tx;
        m_Counters.uptimeSeconds = step / 10;
        return m_Counters;
    }

  private:
    Platform::SystemCounters m_Counters;
};

/// A SystemModel fed from a MockSystemProbe's capabilities and synthetic counters, holding a steady
/// `samples`-long history.
struct SystemFixture
{
    std::unique_ptr<Domain::SystemModel> model;
    SystemCounterFeed feed;
    std::uint64_t step = 0;

    SystemFixture(std::int64_t samples, std::size_t cores, std::size_t interfaces) : feed(cores, interfaces)
    {
        auto probe = std::make_unique<TestMocks::MockSystemProbe>();
        probe->setCapabilities(TestMocks::makeFullSystemCapabilities());
        model = std::make_unique<Domain::SystemModel>(std::move(probe));
        model->setMaxHistorySeconds(windowSecondsFor(samples));
        // One more than the window holds, so the history is full before the first measured sample.
        // Preloaded as one series with one publish (#1413) rather than a publish per sample, which
        // was O(N^2) to build at 18k samples and many series.
        const auto last = static_cast<std::uint64_t>(samples);
        model->updateFromCounterSeries(
            [this, last](Platform::SystemCounters& counters, double& nowSeconds)
            {
                if (step > last)
                {
                    return false;
                }
                counters = feed.at(step);
                nowSeconds = sampleSeconds(step);
                ++step;
                return true;
            });
    }

    [[nodiscard]] static double sampleSeconds(std::uint64_t at) noexcept
    {
        return START_SECONDS + (static_cast<double>(at) * SAMPLE_INTERVAL_SECONDS);
    }

    /// Append, trim and publish one sample.
    void sampleOnce()
    {
        model->updateFromCounters(feed.at(step), sampleSeconds(step));
        ++step;
    }
};

[[nodiscard]] SystemFixture& systemFixture(std::int64_t samples, std::size_t cores, std::size_t interfaces)
{
    return cachedFixture<SystemFixture>(std::tuple{samples, cores, interfaces},
                                        [&] { return std::make_unique<SystemFixture>(samples, cores, interfaces); });
}

void reportSystemShape(benchmark::State& state, const Domain::SystemModel& model)
{
    const auto publication = model.publication();
    state.counters["samples"] = benchmark::Counter(static_cast<double>(publication->timestamps.size()));
    state.counters["cores"] = benchmark::Counter(static_cast<double>(publication->perCoreHistory.size()));
    state.counters["interfaces"] = benchmark::Counter(static_cast<double>(publication->perInterfaceRxHistory.size()));
}

constexpr std::size_t DEFAULT_CORES = 8;
constexpr std::size_t DEFAULT_INTERFACES = 2;

// One SystemModel sample -- append, trim, publish() -- with range(0) samples retained (8 cores, 2
// interfaces). publish() shares every series rather than copying it (#1412), so this no longer
// grows with history length.
void BM_SystemModel_FullHistory_Publish(benchmark::State& state)
{
    SystemFixture& fixture = systemFixture(state.range(0), DEFAULT_CORES, DEFAULT_INTERFACES);
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportSystemShape(state, *fixture.model);
}
BENCHMARK(BM_SystemModel_FullHistory_Publish)
    ->Arg(DEFAULT_HISTORY_SAMPLES)
    ->Arg(DEFAULT_WINDOW_FAST_SAMPLES)
    ->Arg(FULL_HISTORY_SAMPLES)
    ->Unit(benchmark::kMicrosecond);

// The UI's side at full history, uncontended: SystemModel::publication(), a shared_ptr copy under the
// shared lock. It must stay O(1) however long the history is.
void BM_SystemModel_FullHistory_Publication(benchmark::State& state)
{
    const SystemFixture& fixture = systemFixture(state.range(0), DEFAULT_CORES, DEFAULT_INTERFACES);
    for (auto _ : state)
    {
        auto publication = fixture.model->publication();
        benchmark::DoNotOptimize(publication->version);
    }
    reportSystemShape(state, *fixture.model);
}
BENCHMARK(BM_SystemModel_FullHistory_Publication)->Arg(FULL_HISTORY_SAMPLES);

// One SystemModel sample at a 300-sample history with range(0) cores and range(1) interfaces: the
// per-core and per-interface series publish() shares (#1412), and the per-interface maps it rebuilds.
void BM_SystemModel_Cardinality_Publish(benchmark::State& state)
{
    constexpr std::int64_t SAMPLES = DEFAULT_HISTORY_SAMPLES;
    SystemFixture& fixture = systemFixture(SAMPLES, static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportSystemShape(state, *fixture.model);
}
BENCHMARK(BM_SystemModel_Cardinality_Publish)
    ->ArgNames({"cores", "interfaces"})
    ->Args({8, 10})
    ->Args({64, 10})
    ->Args({256, 10})
    ->Args({8, 100})
    ->Args({8, 500})
    ->Unit(benchmark::kMicrosecond);

// One SystemModel sample at range(0) samples retained with range(1) cores and range(2) interfaces: the
// cardinality benchmark's many series at the default and longest histories. Before #1412 publish()
// copied every one of them, so 18k cost about 6x 3k; with shared history the two should match.
void BM_SystemModel_CardinalityHistory_Publish(benchmark::State& state)
{
    SystemFixture& fixture =
        systemFixture(state.range(0), static_cast<std::size_t>(state.range(1)), static_cast<std::size_t>(state.range(2)));
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportSystemShape(state, *fixture.model);
}
BENCHMARK(BM_SystemModel_CardinalityHistory_Publish)
    ->ArgNames({"samples", "cores", "interfaces"})
    ->Args({DEFAULT_WINDOW_FAST_SAMPLES, 64, 10})
    ->Args({FULL_HISTORY_SAMPLES, 64, 10})
    ->Unit(benchmark::kMicrosecond);

// The UI's read side at full history: take the publication and walk every published series once --
// the timestamps, all twelve aggregate series, every per-core series and both per-interface maps --
// as the charts' reductions do. Shared history (#1412) must not make this slower than reading a
// plain vector. Counter: `series`, how many series one iteration walks.
void BM_SystemModel_FullHistory_ReadSeries(benchmark::State& state)
{
    const SystemFixture& fixture = systemFixture(state.range(0), DEFAULT_CORES, DEFAULT_INTERFACES);
    std::size_t seriesWalked = 0;
    for (auto _ : state)
    {
        const auto publication = fixture.model->publication();
        double sum = 0.0;
        std::size_t series = 1; // the timestamps
        for (const double t : publication->timestamps)
        {
            sum += t;
        }
        const auto add = [&sum, &series](const auto& values)
        {
            for (const float value : values)
            {
                sum += static_cast<double>(value);
            }
            ++series;
        };
        add(publication->cpuHistory);
        add(publication->cpuUserHistory);
        add(publication->cpuSystemHistory);
        add(publication->cpuIowaitHistory);
        add(publication->cpuIdleHistory);
        add(publication->memoryHistory);
        add(publication->memoryCachedHistory);
        add(publication->swapHistory);
        add(publication->powerHistory);
        add(publication->batteryChargeHistory);
        add(publication->netRxHistory);
        add(publication->netTxHistory);
        for (const auto& core : publication->perCoreHistory)
        {
            add(core);
        }
        for (const auto& [name, rx] : publication->perInterfaceRxHistory)
        {
            add(rx);
        }
        for (const auto& [name, tx] : publication->perInterfaceTxHistory)
        {
            add(tx);
        }
        benchmark::DoNotOptimize(sum);
        seriesWalked = series;
    }
    reportSystemShape(state, *fixture.model);
    state.counters["series"] = benchmark::Counter(static_cast<double>(seriesWalked));
}
BENCHMARK(BM_SystemModel_FullHistory_ReadSeries)->Arg(FULL_HISTORY_SAMPLES)->Unit(benchmark::kMicrosecond);

/// How long the concurrent benchmark's reader lets the writer run before it asks for the publication:
/// ample for the writer to see its cue and take the model's lock, short against any publish.
constexpr auto CONCURRENT_HEAD_START = std::chrono::microseconds(5);
/// A publication() call faster than this did not wait on the writer (uncontended it is ~20 ns).
constexpr auto CONCURRENT_WAITED = std::chrono::microseconds(1);
/// Fixed iteration count for the concurrent benchmark. Manual time counts only the reader's wait, so
/// once that wait is near zero (#868) the default min-time rule would run millions of iterations, each
/// still paying a full writer sample, and overrun the Heavy Checks timeout.
constexpr benchmark::IterationCount CONCURRENT_ITERATIONS = 2000;

/// Busy-waits until @p deadline: a sleep would hand the core away for far longer than the wait.
void spinUntil(std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline)
    {
    }
}

// How long a UI-style reader waits in a model's publication() while the sampler publishes: the
// baseline #868's critical-section work should shrink. `fixture` has the model in `model` and takes
// one sample with sampleOnce().
//
// Each iteration is one publish met head-on: the reader cues a writer thread to take one sample
// (append, trim and publish(), which copied every series under the model's exclusive lock before
// #868), waits CONCURRENT_HEAD_START for it to take the lock, then calls publication() -- which, on
// the old locking, blocks until the publish is done. The reported time is that call alone (manual
// timing): the lock's hold time less the head start, plus the blocked reader's wake-up -- what a frame
// that lands on a publish stalls for. Handing over this way, rather than running the writer free,
// keeps the measurement about one hold: a writer publishing back to back re-takes the lock before a
// woken reader runs, and the reader then waits on the scheduler for many holds.
//
// range(1) more reader threads call publication() throughout, as other panels would. On a
// reader-preferring rwlock (glibc's default) they can delay the writer's lock past the head start, so
// the measured reader slips in first: that shows as a lower time, `missed_pct`, and a higher
// `writer_sample_us`. Counters:
//   writer_sample_us -- the writer's mean time per sample (append, trim, publish), including taking
//                       the lock; compare the model's uncontended *_Publish benchmark, the same work
//   missed_pct       -- iterations whose publication() didn't wait (the writer hadn't the lock yet)
template<typename Fixture> void measureConcurrentPublicationWait(benchmark::State& state, Fixture& fixture)
{
    auto& model = *fixture.model;

    enum class Phase : std::uint8_t
    {
        Idle,
        Cued,
        Done,
    };
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<std::int64_t> sampleNanos{0};
    std::int64_t samples = 0;
    std::int64_t missed = 0;
    {
        std::jthread writer(
            [&fixture, &phase, &sampleNanos](const std::stop_token& stop)
            {
                while (!stop.stop_requested())
                {
                    if (phase.load(std::memory_order_acquire) != Phase::Cued)
                    {
                        continue; // Spin: a sleeping writer would answer its cue late
                    }
                    const auto start = std::chrono::steady_clock::now();
                    fixture.sampleOnce();
                    const auto took = std::chrono::steady_clock::now() - start;
                    sampleNanos.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(took).count(), std::memory_order_relaxed);
                    phase.store(Phase::Done, std::memory_order_release);
                }
            });
        std::vector<std::jthread> extraReaders;
        extraReaders.reserve(static_cast<std::size_t>(state.range(1)));
        for (std::int64_t i = 0; i < state.range(1); ++i)
        {
            extraReaders.emplace_back(
                [&model](const std::stop_token& stop)
                {
                    while (!stop.stop_requested())
                    {
                        auto publication = model.publication();
                        benchmark::DoNotOptimize(publication->version);
                    }
                });
        }

        for (auto _ : state)
        {
            phase.store(Phase::Cued, std::memory_order_release);
            spinUntil(std::chrono::steady_clock::now() + CONCURRENT_HEAD_START);
            const auto start = std::chrono::steady_clock::now();
            auto publication = model.publication();
            const auto waited = std::chrono::steady_clock::now() - start;
            state.SetIterationTime(std::chrono::duration<double>(waited).count());
            benchmark::DoNotOptimize(publication->version);
            missed += (waited < CONCURRENT_WAITED) ? 1 : 0;
            ++samples;
            while (phase.load(std::memory_order_acquire) != Phase::Done)
            {
            }
            phase.store(Phase::Idle, std::memory_order_relaxed);
        }

        writer.request_stop();
        for (auto& reader : extraReaders)
        {
            reader.request_stop();
        }
        // Join before the counters below read what the threads wrote.
        writer.join();
        extraReaders.clear();
    }
    state.counters["writer_sample_us"] =
        benchmark::Counter((samples == 0) ? 0.0 : (static_cast<double>(sampleNanos.load()) / static_cast<double>(samples)) / 1e3);
    state.counters["missed_pct"] =
        benchmark::Counter((samples == 0) ? 0.0 : (100.0 * static_cast<double>(missed)) / static_cast<double>(samples));
}

// measureConcurrentPublicationWait() on SystemModel at a range(0)-sample history (8 cores, 2
// interfaces), with range(1) extra readers. Compare BM_SystemModel_FullHistory_Publish.
void BM_SystemModel_Concurrent_PublicationWait(benchmark::State& state)
{
    SystemFixture& fixture = systemFixture(state.range(0), DEFAULT_CORES, DEFAULT_INTERFACES);
    measureConcurrentPublicationWait(state, fixture);
    reportSystemShape(state, *fixture.model);
}
BENCHMARK(BM_SystemModel_Concurrent_PublicationWait)
    ->ArgNames({"samples", "extra_readers"})
    ->Args({DEFAULT_WINDOW_FAST_SAMPLES, 0})
    ->Args({FULL_HISTORY_SAMPLES, 0})
    ->Args({FULL_HISTORY_SAMPLES, 2})
    ->Iterations(CONCURRENT_ITERATIONS)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// =============================================================================
// StorageModel
// =============================================================================

/// A StorageModel on a MockDiskProbe whose counters advance every sample, holding a steady
/// `samples`-long history of `disks` disks.
struct StorageFixture
{
    Mocks::MockDiskProbe* probe = nullptr; // Owned by model
    std::unique_ptr<Domain::StorageModel> model;
    Platform::SystemDiskCounters counters;
    std::uint64_t step = 0;

    StorageFixture(std::int64_t samples, std::size_t disks)
    {
        auto ownedProbe = std::make_unique<Mocks::MockDiskProbe>();
        probe = ownedProbe.get();
        model = std::make_unique<Domain::StorageModel>(std::move(ownedProbe));
        model->setMaxHistorySeconds(windowSecondsFor(samples));
        counters.disks.resize(disks);
        for (std::size_t i = 0; i < disks; ++i)
        {
            counters.disks[i].deviceName = "nvme" + std::to_string(i) + "n1";
        }
        // One more than the window holds, so the history is full before the first measured sample.
        // Preloaded as one series with one publish (#1413) rather than a publish per sample, which
        // was O(N^2) to build at 18k samples.
        const auto last = static_cast<std::uint64_t>(samples);
        model->sampleSeries(
            [this, last](Platform::SystemDiskCounters& next, std::chrono::steady_clock::time_point& now)
            {
                if (step > last)
                {
                    return false;
                }
                advance();
                next = counters;
                now = sampleTime(step);
                ++step;
                return true;
            });
    }

    [[nodiscard]] static std::chrono::steady_clock::time_point sampleTime(std::uint64_t at)
    {
        return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(START_SECONDS + (static_cast<double>(at) * SAMPLE_INTERVAL_SECONDS))));
    }

    /// Advance every disk's counters by one sample's I/O.
    void advance()
    {
        for (std::size_t i = 0; i < counters.disks.size(); ++i)
        {
            auto& disk = counters.disks[i];
            disk.readsCompleted += 10 + ((step + i) % 5);
            disk.readSectors += 800 + (((step + i) % 7) * 64);
            disk.writesCompleted += 4 + ((step + i) % 3);
            disk.writeSectors += 300 + (((step + i) % 5) * 32);
            disk.readTimeMs += 2;
            disk.writeTimeMs += 1;
            disk.ioTimeMs += 3;
        }
    }

    /// Append, trim and publish one sample.
    void sampleOnce()
    {
        advance();
        probe->setNextCounters(counters);
        model->sampleAt(sampleTime(step));
        ++step;
    }
};

[[nodiscard]] StorageFixture& storageFixture(std::int64_t samples, std::size_t disks)
{
    return cachedFixture<StorageFixture>(std::pair{samples, disks}, [&] { return std::make_unique<StorageFixture>(samples, disks); });
}

void reportStorageShape(benchmark::State& state, const Domain::StorageModel& model)
{
    const auto publication = model.publication();
    state.counters["samples"] = benchmark::Counter(static_cast<double>(publication->timestamps.size()));
    state.counters["disks"] = benchmark::Counter(static_cast<double>(publication->perDiskHistory.size()));
}

constexpr std::size_t DEFAULT_DISKS = 4;

// One StorageModel sample -- per-disk rates, history append, trim, publish -- with range(0) samples
// retained (4 disks). Before #1412 publish() copied the timestamps, both totals and every per-disk
// series into each publication, so this grew with history length; with shared history it should not.
void BM_StorageModel_FullHistory_Publish(benchmark::State& state)
{
    StorageFixture& fixture = storageFixture(state.range(0), DEFAULT_DISKS);
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportStorageShape(state, *fixture.model);
}
BENCHMARK(BM_StorageModel_FullHistory_Publish)
    ->Arg(DEFAULT_HISTORY_SAMPLES)
    ->Arg(DEFAULT_WINDOW_FAST_SAMPLES)
    ->Arg(FULL_HISTORY_SAMPLES)
    ->Unit(benchmark::kMicrosecond);

// The UI's side at full history, uncontended: StorageModel::publication(). It must stay O(1) however
// long the history is.
void BM_StorageModel_FullHistory_Publication(benchmark::State& state)
{
    const StorageFixture& fixture = storageFixture(state.range(0), DEFAULT_DISKS);
    for (auto _ : state)
    {
        auto publication = fixture.model->publication();
        benchmark::DoNotOptimize(publication->version);
    }
    reportStorageShape(state, *fixture.model);
}
BENCHMARK(BM_StorageModel_FullHistory_Publication)->Arg(FULL_HISTORY_SAMPLES);

// The UI's read side at full history: take the publication and walk every published series once --
// the timestamps, both totals and each disk's read and write series -- as the charts' reductions do.
// Shared history (#1412) must not make this slower than reading a plain vector. Counter: `series`,
// how many series one iteration walks.
void BM_StorageModel_FullHistory_ReadSeries(benchmark::State& state)
{
    const StorageFixture& fixture = storageFixture(state.range(0), DEFAULT_DISKS);
    std::size_t seriesWalked = 0;
    for (auto _ : state)
    {
        const auto publication = fixture.model->publication();
        double sum = 0.0;
        std::size_t series = 0;
        const auto add = [&sum, &series](const auto& values)
        {
            for (const double value : values)
            {
                sum += value;
            }
            ++series;
        };
        add(publication->timestamps);
        add(publication->totalReadHistory);
        add(publication->totalWriteHistory);
        for (const auto& disk : publication->perDiskHistory)
        {
            add(disk.readBytesPerSec);
            add(disk.writeBytesPerSec);
        }
        benchmark::DoNotOptimize(sum);
        seriesWalked = series;
    }
    reportStorageShape(state, *fixture.model);
    state.counters["series"] = benchmark::Counter(static_cast<double>(seriesWalked));
}
BENCHMARK(BM_StorageModel_FullHistory_ReadSeries)->Arg(FULL_HISTORY_SAMPLES)->Unit(benchmark::kMicrosecond);

// One StorageModel sample at a 300-sample history of range(0) disks.
void BM_StorageModel_Cardinality_Publish(benchmark::State& state)
{
    constexpr std::int64_t SAMPLES = DEFAULT_HISTORY_SAMPLES;
    StorageFixture& fixture = storageFixture(SAMPLES, static_cast<std::size_t>(state.range(0)));
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportStorageShape(state, *fixture.model);
}
BENCHMARK(BM_StorageModel_Cardinality_Publish)->ArgName("disks")->Arg(16)->Arg(64)->Unit(benchmark::kMicrosecond);

// One StorageModel sample at range(0) samples retained with range(1) disks: the cardinality
// benchmark's many series at the default-window-fast and longest histories. Before #1412 publish()
// copied every one of them, so 18k cost about 6x 3k; with shared history the two should match.
void BM_StorageModel_CardinalityHistory_Publish(benchmark::State& state)
{
    StorageFixture& fixture = storageFixture(state.range(0), static_cast<std::size_t>(state.range(1)));
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportStorageShape(state, *fixture.model);
}
BENCHMARK(BM_StorageModel_CardinalityHistory_Publish)
    ->ArgNames({"samples", "disks"})
    ->Args({DEFAULT_WINDOW_FAST_SAMPLES, 16})
    ->Args({FULL_HISTORY_SAMPLES, 16})
    ->Unit(benchmark::kMicrosecond);

// measureConcurrentPublicationWait() on StorageModel at a range(0)-sample history of 4 disks, with
// range(1) extra readers. Compare BM_StorageModel_FullHistory_Publish.
void BM_StorageModel_Concurrent_PublicationWait(benchmark::State& state)
{
    StorageFixture& fixture = storageFixture(state.range(0), DEFAULT_DISKS);
    measureConcurrentPublicationWait(state, fixture);
    reportStorageShape(state, *fixture.model);
}
BENCHMARK(BM_StorageModel_Concurrent_PublicationWait)
    ->ArgNames({"samples", "extra_readers"})
    ->Args({FULL_HISTORY_SAMPLES, 0})
    ->Args({FULL_HISTORY_SAMPLES, 2})
    ->Iterations(CONCURRENT_ITERATIONS)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// =============================================================================
// GPUModel
// =============================================================================

/// A GPUModel on a MockGPUProbe of `gpus` GPUs whose utilization varies every sample, holding a steady
/// `samples`-long history. A mock, so it runs (and measures the same thing) on a GPU-less runner too.
struct GPUFixture
{
    TestMocks::MockGPUProbe* probe = nullptr; // Owned by model
    std::unique_ptr<Domain::GPUModel> model;
    std::vector<std::string> gpuIds;
    std::uint64_t step = 0;

    GPUFixture(std::int64_t samples, std::size_t gpus)
    {
        auto ownedProbe = std::make_unique<TestMocks::MockGPUProbe>();
        for (std::size_t i = 0; i < gpus; ++i)
        {
            gpuIds.push_back("GPU" + std::to_string(i));
            ownedProbe->withGPU(gpuIds.back(), "Synthetic GPU " + std::to_string(i));
        }
        probe = ownedProbe.get();
        model = std::make_unique<Domain::GPUModel>(std::move(ownedProbe));
        model->setMaxHistorySeconds(windowSecondsFor(samples));
        for (std::int64_t i = 0; i <= samples; ++i)
        {
            sampleOnce();
        }
    }

    /// Append, trim and publish one sample.
    void sampleOnce()
    {
        for (std::size_t i = 0; i < gpuIds.size(); ++i)
        {
            probe->withUtilization(gpuIds[i], static_cast<double>((step + (i * 17)) % 100));
        }
        const auto now = std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(START_SECONDS + (static_cast<double>(step) * SAMPLE_INTERVAL_SECONDS))));
        model->refreshAt(now);
        ++step;
    }
};

/// GPUs in the GPU fixtures: a discrete card and an integrated one, a common pairing.
constexpr std::size_t DEFAULT_GPUS = 2;

[[nodiscard]] GPUFixture& gpuFixture(std::int64_t samples, std::size_t gpus)
{
    return cachedFixture<GPUFixture>(std::pair{samples, gpus}, [&] { return std::make_unique<GPUFixture>(samples, gpus); });
}

void reportGpuShape(benchmark::State& state, const Domain::GPUModel& model)
{
    const auto publication = model.publication();
    const auto first = publication->histories.begin();
    state.counters["samples"] =
        benchmark::Counter((first == publication->histories.end()) ? 0.0 : static_cast<double>(first->second.timestamps.size()));
    state.counters["gpus"] = benchmark::Counter(static_cast<double>(publication->histories.size()));
}

// One GPUModel sample -- snapshots, history append, trim, publish -- at a range(0)-sample history of 2
// GPUs. publish() derives 11 series per GPU from the history. Held at the 5-minute window at 100 ms
// (3000), not the 18k maximum: GPUModel has no batch path, so building an 18k window takes 18k
// publishes, far longer than the measurement.
void BM_GPUModel_History_Publish(benchmark::State& state)
{
    GPUFixture& fixture = gpuFixture(state.range(0), DEFAULT_GPUS);
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    reportGpuShape(state, *fixture.model);
}
BENCHMARK(BM_GPUModel_History_Publish)->Arg(DEFAULT_WINDOW_FAST_SAMPLES)->Unit(benchmark::kMicrosecond);

// measureConcurrentPublicationWait() on GPUModel at a range(0)-sample history of 2 GPUs, with
// range(1) extra readers (#868). Compare BM_GPUModel_History_Publish.
void BM_GPUModel_Concurrent_PublicationWait(benchmark::State& state)
{
    GPUFixture& fixture = gpuFixture(state.range(0), DEFAULT_GPUS);
    measureConcurrentPublicationWait(state, fixture);
    reportGpuShape(state, *fixture.model);
}
BENCHMARK(BM_GPUModel_Concurrent_PublicationWait)
    ->ArgNames({"samples", "extra_readers"})
    ->Args({DEFAULT_WINDOW_FAST_SAMPLES, 0})
    ->Args({DEFAULT_WINDOW_FAST_SAMPLES, 2})
    ->Iterations(CONCURRENT_ITERATIONS)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// =============================================================================
// ProcessModel
// =============================================================================

/// A ProcessModel on a MockProcessProbe fed `processes` synthetic processes, with an injected clock
/// advanced one sample interval per refresh.
struct ProcessFixture
{
    std::unique_ptr<Domain::ProcessModel> model;
    std::vector<Platform::ProcessCounters> counters;
    std::shared_ptr<Domain::ProcessModel::Clock::time_point> now =
        std::make_shared<Domain::ProcessModel::Clock::time_point>(std::chrono::seconds(static_cast<int>(START_SECONDS)));
    std::uint64_t totalCpuTime = 0;
    std::uint64_t step = 0;

    explicit ProcessFixture(std::size_t processes)
    {
        model = std::make_unique<Domain::ProcessModel>(std::make_unique<TestMocks::MockProcessProbe>(), [clock = now] { return *clock; });
        counters.reserve(processes);
        for (std::size_t i = 0; i < processes; ++i)
        {
            const auto pid = static_cast<std::int32_t>(100 + i);
            counters.push_back(
                TestMocks::makeProcessCounters(pid, "proc" + std::to_string(i), 'S', 0, 0, 1000 + i, (4ULL + (i % 64)) << 20U));
        }
        sampleOnce(); // Seeds the previous counters, so later samples compute deltas
    }

    void sampleOnce()
    {
        for (std::size_t i = 0; i < counters.size(); ++i)
        {
            auto& process = counters[i];
            process.userTime += (step + i) % 3;
            process.systemTime += (step + i) % 2;
        }
        totalCpuTime += 1000;
        *now += std::chrono::milliseconds(100);
        model->updateFromCounters(counters, totalCpuTime);
        ++step;
    }
};

// One ProcessModel refresh -- per-process deltas and the published snapshot vector -- with range(0)
// processes.
void BM_ProcessModel_Cardinality_Publish(benchmark::State& state)
{
    auto& fixture = cachedFixture<ProcessFixture>(
        state.range(0), [&] { return std::make_unique<ProcessFixture>(static_cast<std::size_t>(state.range(0))); });
    for (auto _ : state)
    {
        fixture.sampleOnce();
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(fixture.model->snapshots().size()));
}
BENCHMARK(BM_ProcessModel_Cardinality_Publish)->ArgName("processes")->Arg(500)->Arg(2'000)->Arg(10'000)->Unit(benchmark::kMicrosecond);

} // namespace
