// Benchmarks for the Windows process sampler while processes start and exit (Windows only, #2982)
//
// bench_WindowsProcessProbe.cpp times the probe over the processes already running, and
// bench_WindowsSignature.cpp times the Signature column's worker with a fake check. Neither has new
// processes arriving, so neither pays what a process start costs a sample: its first detail reads (the
// image path and publisher, read once per {pid, start time}; the owner, the command line, the package)
// and, for an image not seen before, the request that sends its path to the signature worker (#3070,
// #3340). These do, with no UI:
//
//  - Churn_Enumerate/starts:S/new_paths:N: the real WindowsProcessProbe::enumerate() with the detail
//    caches' clock moved on 1 s a call (the default refresh interval: the light details every sample,
//    the heavy ones every fourth on a 16 GB+ machine), and before each call, untimed, S processes
//    started and the S started CHURN_SAMPLES_ALIVE samples earlier ended. N = 0 starts them all from
//    one copy of a small System32 executable (new PIDs of an image already checked, as a browser's or a
//    build's helpers are); N = 1 copies the executable to a new path for each start, so every start is
//    an image the signature worker hasn't seen (a build's or a test run's fresh binaries).
//  - Churn_Sampler/starts:S/new_paths:N: the same churn, timing ProcessModel::refresh() over that probe:
//    the process sampler's whole pass (enumerate(), the probe's other reads, the Domain update and the
//    published snapshot), as the sampler thread runs it, without the UI. off_thread_cpu_per_start is the
//    CPU time this process spent on threads other than the sampler's (the signature worker), from the
//    first sample until the worker has gone quiet, for each process started.
//  - Churn_Attribution/starts:S/new_paths:N: Churn_Enumerate with each system call the probe makes
//    through its seams (WindowsProcessProbeFunctions) timed on the way through, reported as the mean
//    microseconds a sample spends in it: the snapshot, OpenProcess, the image path, the command line and
//    the other NtQueryInformationProcess classes, the token and account reads, the package, the
//    sample's window walk (#3705: the Not responding check and the window AppIDs both read it; two walks
//    before), the window AppID lookup, and the rest. unattributed is the sample's
//    wall time less all of them: the probe's own code and what isn't behind a seam (the publisher's
//    GetFileVersionInfo, the signature worker's request).
//  - FirstEnumerate/iterations:1 and /iterations:10: a new probe built and its first enumerate(), as the
//    process sampler's start hook makes them before the Processes table's first rows (#3714), with the same
//    seams timed (plus the app names and the network detection) and construct_us the constructor's share.
//    iterations:1 runs first in a benchmark process, with every per-process cache cold, as at launch;
//    iterations:10 is ten more fresh probes in the same process.
//
// The processes are started suspended (CREATE_SUSPENDED) and ended before they run, so the churn spends
// no CPU of its own beyond the kernel's process creation and exit, which falls outside the timed
// samples. The wall and CPU times are the sampler thread's; cpu_time is its CPU in the timed samples.
// Live-probe benchmarks: the machine's own processes, disk and load are in every figure, so compare runs
// as a relative A/B on one machine, never against a threshold.

#if defined(_WIN32)

#include "Domain/ProcessModel.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessProbe.h"

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <objbase.h>
#include <windows.h>

namespace
{

/// The samples each repetition times: enough for every churned process to be read on a light and a heavy
/// pass, few enough that a run of new paths stays short.
constexpr std::int64_t CHURN_SAMPLES = 40;
/// How many samples a churned process lives: one heavy TTL at the 1 s interval on a 16 GB+ machine, so it
/// is seen at its first read and on the light passes after it, and ends before its second heavy read.
constexpr std::size_t CHURN_SAMPLES_ALIVE = 4;
constexpr auto CHURN_SAMPLE_STEP = std::chrono::milliseconds{1000};
/// Off-thread CPU is read until it stops rising for this long, at most CHURN_DRAIN_LIMIT.
constexpr auto CHURN_DRAIN_QUIET = std::chrono::milliseconds{500};
constexpr auto CHURN_DRAIN_LIMIT = std::chrono::seconds{10};
constexpr int DELETE_ATTEMPTS = 20;
constexpr auto DELETE_RETRY = std::chrono::milliseconds{50};
constexpr double TICKS_PER_MICROSECOND = 10.0; // FILETIME's 100 ns ticks

/// The detail caches' clock (WindowsProcessProbeFunctions::system.steadyNow), moved on by hand.
[[nodiscard]] std::chrono::steady_clock::time_point& fakeNow()
{
    static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    return now;
}

[[nodiscard]] std::chrono::steady_clock::time_point readFakeNow()
{
    return fakeNow();
}

/// The system's functions, the signature worker included, but the detail caches read fakeNow() and no
/// energy meter is opened (no opener thread runs beside the benchmark).
[[nodiscard]] Platform::WindowsProcessProbeFunctions fakeClockFunctions()
{
    auto functions = Platform::WindowsProcessProbe::systemFunctions();
    functions.system.steadyNow = &readFakeNow;
    functions.energyMeter = std::nullopt;
    return functions;
}

[[nodiscard]] std::uint64_t ticks(const FILETIME& time)
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
}

/// This process's CPU time (kernel + user) less the calling thread's, in 100 ns ticks: what its other
/// threads have spent.
[[nodiscard]] std::uint64_t otherThreadsCpuTicks()
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == 0)
    {
        return 0;
    }
    const std::uint64_t process = ticks(kernel) + ticks(user);
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) == 0)
    {
        return 0;
    }
    const std::uint64_t thread = ticks(kernel) + ticks(user);
    return process > thread ? process - thread : 0;
}

/// Waits, up to CHURN_DRAIN_LIMIT, until the other threads' CPU time has stopped rising for
/// CHURN_DRAIN_QUIET (the signature worker has checked what it was given), and returns it then.
[[nodiscard]] std::uint64_t otherThreadsCpuTicksOnceQuiet()
{
    const auto limit = std::chrono::steady_clock::now() + CHURN_DRAIN_LIMIT;
    std::uint64_t last = otherThreadsCpuTicks();
    auto quietSince = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < limit)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const std::uint64_t now = otherThreadsCpuTicks();
        if (now != last)
        {
            last = now;
            quietSince = std::chrono::steady_clock::now();
        }
        else if (std::chrono::steady_clock::now() - quietSince >= CHURN_DRAIN_QUIET)
        {
            break;
        }
    }
    return last;
}

/// The executable the churn starts copies of: a small catalog-signed System32 file.
[[nodiscard]] std::wstring churnSourceImage()
{
    std::wstring directory(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size())
    {
        return {};
    }
    directory.resize(length);
    for (const wchar_t* name : {L"\\whoami.exe", L"\\cmd.exe"})
    {
        std::wstring path = directory + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            return path;
        }
    }
    return {};
}

/// Deletes @p path, retrying while the image of a process just ended is still being released.
void deleteWithRetry(const std::wstring& path)
{
    for (int attempt = 0; attempt < DELETE_ATTEMPTS; ++attempt)
    {
        if (DeleteFileW(path.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            return;
        }
        std::this_thread::sleep_for(DELETE_RETRY);
    }
}

/// Starts suspended processes from copies of churnSourceImage() in a directory
/// of its own under %TEMP%, and ends each CHURN_SAMPLES_ALIVE samples after it
/// started. The copies stay until the end, as a build's binaries stay on disk
/// after they ran, so the signature worker checks each one whenever it gets to
/// it. Ends every process, and deletes the copies and the directory, when
/// destroyed.
class ProcessChurn
{
  public:
    explicit ProcessChurn(bool newPaths) : m_NewPaths(newPaths), m_Source(churnSourceImage())
    {
        std::wstring temp(MAX_PATH + 1, L'\0');
        const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (m_Source.empty() || length == 0 || length >= temp.size())
        {
            return;
        }
        temp.resize(length);
        // A directory per run: the publisher cache (per path, for the process's life) must not know a path
        // from an earlier repetition.
        static std::size_t runs = 0;
        m_Directory = temp + L"TaskSmackChurn-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(runs++);
        if (CreateDirectoryW(m_Directory.c_str(), nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            m_Directory.clear();
            return;
        }
        m_Ready = m_NewPaths || !copyImage().empty(); // Of one image: the one copy every start runs
    }

    ~ProcessChurn()
    {
        while (!m_Samples.empty())
        {
            endOldestSample();
        }
        for (const std::wstring& image : m_Images)
        {
            deleteWithRetry(image);
        }
        if (!m_Directory.empty())
        {
            (void) RemoveDirectoryW(m_Directory.c_str());
        }
    }

    ProcessChurn(const ProcessChurn&) = delete;
    ProcessChurn& operator=(const ProcessChurn&) = delete;
    ProcessChurn(ProcessChurn&&) = delete;
    ProcessChurn& operator=(ProcessChurn&&) = delete;

    [[nodiscard]] bool ready() const
    {
        return m_Ready;
    }

    /// The processes started so far.
    [[nodiscard]] std::size_t started() const
    {
        return m_Started;
    }

    /// One sample's churn: ends the processes started CHURN_SAMPLES_ALIVE samples
    /// ago and starts @p count.
    void nextSample(std::size_t count)
    {
        if (m_Samples.size() >= CHURN_SAMPLES_ALIVE)
        {
            endOldestSample();
        }
        std::vector<Platform::Windows::UniqueHandle> sample;
        sample.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::wstring image = m_NewPaths ? copyImage() : m_Images.front();
            if (image.empty())
            {
                continue;
            }
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION info{};
            if (CreateProcessW(image.c_str(),
                               nullptr,
                               nullptr,
                               nullptr,
                               FALSE,
                               CREATE_SUSPENDED | DETACHED_PROCESS,
                               nullptr,
                               m_Directory.c_str(),
                               &startup,
                               &info) == 0)
            {
                continue;
            }
            (void) CloseHandle(info.hThread);
            sample.emplace_back(info.hProcess);
            ++m_Started;
        }
        m_Samples.push_back(std::move(sample));
    }

  private:
    /// A new copy of the source image, at a path not used before in this run;
    /// empty if it can't be made.
    [[nodiscard]] std::wstring copyImage()
    {
        std::wstring path = m_Directory + L"\\churn-" + std::to_wstring(m_Images.size()) + L".exe";
        if (CopyFileW(m_Source.c_str(), path.c_str(), TRUE) == 0)
        {
            return {};
        }
        m_Images.push_back(path);
        return path;
    }

    void endOldestSample()
    {
        for (const Platform::Windows::UniqueHandle& process : m_Samples.front())
        {
            (void) TerminateProcess(process.get(), 0);
            (void) WaitForSingleObject(process.get(), INFINITE);
        }
        m_Samples.pop_front();
    }

    bool m_NewPaths = false;
    bool m_Ready = false;
    std::wstring m_Source;
    std::wstring m_Directory;
    std::vector<std::wstring> m_Images; ///< Every copy made, deleted at the end
    std::size_t m_Started = 0;
    std::deque<std::vector<Platform::Windows::UniqueHandle>> m_Samples;
};

/// One churn run: @p sample is timed once per iteration, after that sample's
/// churn (untimed).
template<typename Sample> void benchmarkChurn(benchmark::State& state, Sample sample)
{
    const auto starts = static_cast<std::size_t>(state.range(0));
    ProcessChurn churn(state.range(1) != 0);
    if (!churn.ready())
    {
        state.SkipWithError("Could not copy a System32 executable to %TEMP%");
        return;
    }
    // The seed's own signature checks (every running image) finish first, so they
    // neither compete with the timed samples nor count as the churn's.
    const std::uint64_t otherCpuBefore = otherThreadsCpuTicksOnceQuiet();
    std::size_t processes = 0;
    for (auto _ : state)
    {
        state.PauseTiming();
        fakeNow() += CHURN_SAMPLE_STEP;
        churn.nextSample(starts);
        state.ResumeTiming();
        processes = sample();
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(processes));
    state.counters["started"] = benchmark::Counter(static_cast<double>(churn.started()));
    if (churn.started() != 0)
    {
        const std::uint64_t otherCpu = otherThreadsCpuTicksOnceQuiet() - otherCpuBefore;
        state.counters["off_thread_cpu_per_start_us"] =
            benchmark::Counter(static_cast<double>(otherCpu) / TICKS_PER_MICROSECOND / static_cast<double>(churn.started()));
    }
}

void BM_WindowsProcessProbe_Churn_Enumerate(benchmark::State& state)
{
    Platform::WindowsProcessProbe probe(fakeClockFunctions());
    (void) probe.enumerate(); // Every running process's details read once, as a
                              // first sample does
    benchmarkChurn(state,
                   [&probe]
                   {
                       auto counters = probe.enumerate();
                       benchmark::DoNotOptimize(counters.data());
                       return counters.size();
                   });
}

void BM_WindowsProcessProbe_Churn_Sampler(benchmark::State& state)
{
    Domain::ProcessModel model(std::make_unique<Platform::WindowsProcessProbe>(fakeClockFunctions()));
    model.refresh(); // The sampler's seed (ProcessesPanel's start hook): every
                     // process's details read once
    benchmarkChurn(state,
                   [&model]
                   {
                       model.refresh();
                       return model.processCount();
                   });
}

void churnArguments(benchmark::internal::Benchmark* benchmark)
{
    benchmark->ArgNames({"starts", "new_paths"});
    // {starts per sample, new paths}: none, a desktop's few, a build's burst; of
    // one image, then of new ones.
    constexpr std::array<std::pair<std::int64_t, std::int64_t>, 5> ARGUMENTS{{{0, 0}, {2, 0}, {8, 0}, {2, 1}, {8, 1}}};
    for (const auto& [starts, newPaths] : ARGUMENTS)
    {
        benchmark->Args({starts, newPaths});
    }
    benchmark->Iterations(CHURN_SAMPLES)->Unit(benchmark::kMicrosecond);
}

BENCHMARK(BM_WindowsProcessProbe_Churn_Enumerate)->Apply(churnArguments);
BENCHMARK(BM_WindowsProcessProbe_Churn_Sampler)->Apply(churnArguments);

/// The seams Churn_Attribution times, each its own bucket (an index into seamTicks()).
enum class Seam : std::uint8_t
{
    Snapshot,
    OpenProcess,
    ImagePath,
    CommandLine,
    NtQueryOther,
    ProcessInformation,
    GuiResources,
    Affinity,
    PriorityClass,
    Machine,
    Token,
    LookupAccountSid,
    Package,
    WindowAppIds,
    WindowWalk,
    OpenProcessRefused,
    AppDisplayName,
    Network,
    Count,
};

constexpr std::size_t SEAM_COUNT = static_cast<std::size_t>(Seam::Count);

constexpr std::array<const char*, SEAM_COUNT> SEAM_NAMES{
    "snapshot_us",
    "open_process_us",
    "image_path_us",
    "command_line_us",
    "nt_query_other_us",
    "process_information_us",
    "gui_resources_us",
    "affinity_us",
    "priority_class_us",
    "machine_us",
    "token_us",
    "lookup_account_sid_us",
    "package_us",
    "window_app_ids_us",
    "window_walk_us",
    "open_process_refused_us",
    "app_display_name_us",
    "network_us",
};

constexpr ULONG COMMAND_LINE_CLASS = 60; // ProcessCommandLineInformation, as getProcessCommandLine() asks

/// The QueryPerformanceCounter ticks each seam has spent since the last reset (the sampler thread only).
[[nodiscard]] std::array<std::int64_t, SEAM_COUNT>& seamTicks()
{
    static std::array<std::int64_t, SEAM_COUNT> ticks{};
    return ticks;
}

/// Adds @p ticks to @p seam's bucket.
void addSeamTicks(Seam seam, std::int64_t ticks)
{
    seamTicks()[static_cast<std::size_t>(seam)] += ticks;
}

[[nodiscard]] std::int64_t performanceCounter()
{
    LARGE_INTEGER value{};
    (void) QueryPerformanceCounter(&value);
    return value.QuadPart;
}

/// Stands in for one seam's function: calls the real one and adds the time it took to its bucket.
template<Seam Bucket, typename Fn> struct TimedSeam;
template<Seam Bucket, typename R, typename... Args> struct TimedSeam<Bucket, R (*)(Args...)>
{
    static inline R (*real)(Args...) = nullptr;

    static R call(Args... args)
    {
        const std::int64_t start = performanceCounter();
        if constexpr (std::is_void_v<R>)
        {
            real(args...);
            addSeamTicks(Bucket, performanceCounter() - start);
        }
        else
        {
            R result = real(args...);
            addSeamTicks(Bucket, performanceCounter() - start);
            return result;
        }
    }
};

/// Routes @p fn through TimedSeam<Bucket> (a null seam stays null).
template<Seam Bucket, typename Fn> void timeSeam(Fn& fn)
{
    if (fn != nullptr)
    {
        TimedSeam<Bucket, Fn>::real = fn;
        fn = &TimedSeam<Bucket, Fn>::call;
    }
}

/// NtQueryInformationProcess, split by class: the command line apart from the rest.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the real call, set once before the probe is built
LONG(NTAPI* realNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG) = nullptr;

LONG NTAPI timedNtQueryInformationProcess(HANDLE process, ULONG informationClass, PVOID information, ULONG length, PULONG returned)
{
    const std::int64_t start = performanceCounter();
    const LONG status = realNtQueryInformationProcess(process, informationClass, information, length, returned);
    addSeamTicks(informationClass == COMMAND_LINE_CLASS ? Seam::CommandLine : Seam::NtQueryOther, performanceCounter() - start);
    return status;
}

/// OpenProcess, split by outcome: the handles that open apart from the processes that refuse one.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the real call, set once before the probe is built
HANDLE(WINAPI* realOpenProcess)(DWORD, BOOL, DWORD) = nullptr;

HANDLE WINAPI timedOpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    const std::int64_t start = performanceCounter();
    HANDLE handle = realOpenProcess(access, inherit, pid);
    addSeamTicks(handle != nullptr ? Seam::OpenProcess : Seam::OpenProcessRefused, performanceCounter() - start);
    return handle;
}

/// fakeClockFunctions() with every seam Churn_Attribution and FirstEnumerate time routed through its bucket.
[[nodiscard]] Platform::WindowsProcessProbeFunctions timedFunctions()
{
    auto functions = fakeClockFunctions();
    timeSeam<Seam::Snapshot>(functions.system.ntQuerySystemInformation);
    timeSeam<Seam::WindowWalk>(functions.system.topLevelWindows);
    timeSeam<Seam::Token>(functions.token.openProcessToken);
    timeSeam<Seam::Token>(functions.token.getTokenInformation);
    timeSeam<Seam::LookupAccountSid>(functions.token.lookupAccountSidW);
    if (functions.process.openProcess != nullptr)
    {
        realOpenProcess = functions.process.openProcess;
        functions.process.openProcess = &timedOpenProcess;
    }
    timeSeam<Seam::PriorityClass>(functions.process.getPriorityClass);
    timeSeam<Seam::ImagePath>(functions.process.queryFullProcessImageNameW);
    timeSeam<Seam::ProcessInformation>(functions.process.getProcessInformation);
    timeSeam<Seam::GuiResources>(functions.process.getGuiResources);
    timeSeam<Seam::Affinity>(functions.process.getProcessAffinityMask);
    timeSeam<Seam::Affinity>(functions.process.getProcessGroupAffinity);
    timeSeam<Seam::Affinity>(functions.process.openThread);
    timeSeam<Seam::Affinity>(functions.process.getThreadGroupAffinity);
    timeSeam<Seam::Machine>(functions.process.processMachine);
    timeSeam<Seam::Package>(functions.app.packageIdentity);
    timeSeam<Seam::WindowAppIds>(functions.app.windowAppIds);
    if (functions.process.ntQueryInformationProcess != nullptr)
    {
        realNtQueryInformationProcess = functions.process.ntQueryInformationProcess;
        functions.process.ntQueryInformationProcess = &timedNtQueryInformationProcess;
    }
    timeSeam<Seam::AppDisplayName>(functions.app.appDisplayName);
    timeSeam<Seam::Network>(functions.network.loadTcpEStats);
    timeSeam<Seam::Network>(functions.network.getExtendedTcpTable);
    return functions;
}

[[nodiscard]] double ticksToMicroseconds(std::int64_t ticks)
{
    LARGE_INTEGER frequency{};
    (void) QueryPerformanceFrequency(&frequency);
    return static_cast<double>(ticks) * 1e6 / static_cast<double>(frequency.QuadPart);
}

void BM_WindowsProcessProbe_Churn_Attribution(benchmark::State& state)
{
    const auto functions = timedFunctions();
    Platform::WindowsProcessProbe probe(functions);
    (void) probe.enumerate(); // Every running process's details read once, as a first sample does
    seamTicks().fill(0);
    std::int64_t sampleTicks = 0;
    benchmarkChurn(state,
                   [&probe, &sampleTicks]
                   {
                       const std::int64_t start = performanceCounter();
                       auto counters = probe.enumerate();
                       sampleTicks += performanceCounter() - start;
                       benchmark::DoNotOptimize(counters.data());
                       return counters.size();
                   });

    LARGE_INTEGER frequency{};
    (void) QueryPerformanceFrequency(&frequency);
    const auto microseconds = [&frequency](std::int64_t ticks)
    {
        return static_cast<double>(ticks) * 1e6 / static_cast<double>(frequency.QuadPart);
    };
    std::int64_t attributed = 0;
    for (std::size_t seam = 0; seam < SEAM_COUNT; ++seam)
    {
        attributed += seamTicks()[seam];
        state.counters[SEAM_NAMES[seam]] = benchmark::Counter(microseconds(seamTicks()[seam]), benchmark::Counter::kAvgIterations);
    }
    state.counters["unattributed_us"] = benchmark::Counter(microseconds(sampleTicks - attributed), benchmark::Counter::kAvgIterations);
}
BENCHMARK(BM_WindowsProcessProbe_Churn_Attribution)
    ->ArgNames({"starts", "new_paths"})
    ->Args({0, 0})
    ->Args({8, 0})
    ->Args({8, 1})
    ->Iterations(CHURN_SAMPLES)
    ->Unit(benchmark::kMicrosecond);

/// What the process sampler's start hook pays for the probe (#3714): a new WindowsProcessProbe built and its
/// first enumerate(), which reads every process's details at once, on a thread in the multithreaded
/// apartment as the sampler's is (#2731). Each iteration is a fresh probe; its destruction (which joins the
/// signature worker its first sample started) is untimed. construct_us is the constructor's share, and the
/// seam counters are per first pass, as Churn_Attribution's are per sample.
void BM_WindowsProcessProbe_FirstEnumerate(benchmark::State& state)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto functions = timedFunctions();
    seamTicks().fill(0);
    std::int64_t constructTicks = 0;
    std::int64_t passTicks = 0;
    std::size_t processes = 0;
    for (auto _ : state)
    {
        const std::int64_t start = performanceCounter();
        auto probe = std::make_unique<Platform::WindowsProcessProbe>(functions);
        const std::int64_t built = performanceCounter();
        auto counters = probe->enumerate();
        const std::int64_t done = performanceCounter();
        constructTicks += built - start;
        passTicks += done - start;
        processes = counters.size();
        benchmark::DoNotOptimize(counters.data());
        state.PauseTiming();
        probe.reset();
        state.ResumeTiming();
    }
    std::int64_t attributed = 0;
    for (std::size_t seam = 0; seam < SEAM_COUNT; ++seam)
    {
        attributed += seamTicks()[seam];
        state.counters[SEAM_NAMES[seam]] = benchmark::Counter(ticksToMicroseconds(seamTicks()[seam]), benchmark::Counter::kAvgIterations);
    }
    state.counters["construct_us"] = benchmark::Counter(ticksToMicroseconds(constructTicks), benchmark::Counter::kAvgIterations);
    state.counters["unattributed_us"] = benchmark::Counter(ticksToMicroseconds(passTicks - attributed), benchmark::Counter::kAvgIterations);
    state.counters["processes"] = benchmark::Counter(static_cast<double>(processes));
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
}
// The first pass in a benchmark process (every static cache cold, as at launch: the publisher cache is per process),
// then ten more fresh probes in the same process.
BENCHMARK(BM_WindowsProcessProbe_FirstEnumerate)->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_WindowsProcessProbe_FirstEnumerate)->Iterations(10)->Unit(benchmark::kMillisecond);

/// The publisher's first read of an image (WindowsProcessDetailReads::publisher(): GetFileVersionInfo),
/// which the probe makes on the sampler thread with a new process's first heavy read, once per path for
/// the process's life: each iteration reads a copy of the churn's executable at a path never read before
/// (the copy made untimed).
void BM_WindowsProcessProbe_Churn_PublisherFirstRead(benchmark::State& state)
{
    const std::wstring source = churnSourceImage();
    std::wstring temp(MAX_PATH + 1, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (source.empty() || length == 0 || length >= temp.size())
    {
        state.SkipWithError("No System32 executable or %TEMP%");
        return;
    }
    temp.resize(length);
    static std::size_t runs = 0;
    const std::wstring stem =
        temp + L"TaskSmackPublisher-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(runs++) + L"-";
    std::vector<std::wstring> copies;
    for (auto _ : state)
    {
        state.PauseTiming();
        const std::wstring path = stem + std::to_wstring(copies.size()) + L".exe";
        const bool copied = CopyFileW(source.c_str(), path.c_str(), TRUE) != 0;
        if (copied)
        {
            copies.push_back(path);
        }
        state.ResumeTiming();
        if (!copied)
        {
            state.SkipWithError("Could not copy the executable to %TEMP%");
            break;
        }
        auto publisher = Platform::WindowsProcessDetailReads::publisher(path);
        benchmark::DoNotOptimize(publisher);
    }
    for (const std::wstring& copy : copies)
    {
        deleteWithRetry(copy);
    }
}
BENCHMARK(BM_WindowsProcessProbe_Churn_PublisherFirstRead)->Iterations(CHURN_SAMPLES * 4)->Unit(benchmark::kMicrosecond);

} // namespace

#endif // defined(_WIN32)
