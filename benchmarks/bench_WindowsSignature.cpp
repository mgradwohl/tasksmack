// Benchmarks for the Processes table's Signature column (Windows only, #3415): the process probe's
// signature worker (WindowsSignatureWorker.h, #3070, #3340, #3403) and the check it runs
// (WindowsImageSignature.h's checkSignature() over WindowsSignatureCheck.h's system calls, #1661, #3292).
//
// The v0.10.0 review asked for executable-path churn to be profiled: the worker does its work when new
// image paths appear. Two kinds of benchmark here, kept apart:
//
//  - Worker_*: the worker's own code, with a fake check (no file is opened, no WinVerifyTrust call), so
//    they are deterministic and time only TaskSmack's queueing, admission, caching and hand-over. A
//    SimulatedProbe makes the probe's calls (WindowsProcessProbe::updateSignature()): beginSample() once
//    a sample, then for each process whose signature isn't in yet, request() the first time and find()
//    every sample until answered; an answered process isn't asked again. The pause after each file is 0
//    (the product's is SIGNATURE_CHECK_PAUSE_MS), so a sample's new paths are drained at once, and each
//    iteration waits for the worker to finish them: real time, since the checks run on the worker's thread.
//     - Worker_Churn/paths:N/churn_pct:C/max_paths:M: one sample of N processes, every one answered
//       beforehand, with C% of them replaced by processes of images never seen before (a build's
//       compilers, a test run's binaries), in a path table of M (4096 is the product's bound). per_new_path
//       is the sample's time for each new path; unanswered is how many processes are still waiting once
//       one more (untimed) sample has taken the last one's answers: 0 while the table keeps up. The table
//       turns paths away when every one in it is queued (#3340), so a table smaller than a sample's new
//       paths leaves some for later samples (#3403), and under 100% churn most are never checked.
//     - Worker_FirstSample/paths:N/max_paths:M: a new worker and N processes it has never seen, sampled
//       until every one is answered, as the probe's first samples are; the worker's thread start included.
//       samples is how many samples that took (1 when the table holds them all, more when it doesn't).
//     - Worker_Saturated/paths:N/max_paths:M: the table full of queued paths (the worker held on one
//       check), as it is when new images arrive faster than the product's paced worker checks them; one
//       sample of N processes of new images, every one turned away. per_new_path is what each costs the
//       sampler's thread then.
//  - Verify*, CacheHit and FirstSampleBurst: the real check, with TaskSmack's own WinVerifyTrust flags
//    (offline, no revocation check). **Environment-dependent**: the file's size, the disk, the catalog
//    database, Defender, the machine's load. Warm only: the first check in a process loads wintrust and
//    opens the catalog database (hundreds of milliseconds), and is made before any of these time.
//    Each runs few iterations, so a run stays short. Keep them out of any regression threshold, as the
//    other live-probe benchmarks are.
//     - VerifyCatalog: one check of a catalog-signed System32 file (notepad.exe, else cmd.exe,
//       kernel32.dll): the embedded check fails, then the catalog's hash lookup and WinVerifyTrust.
//     - VerifyEmbedded: one check of an Authenticode-signed System32 file (vcruntime140.dll, else
//       msvcp140.dll; they come with the Visual C++ runtime, so a machine without either skips this).
//     - Which file each picked is checked by checkSignature() itself; it is the label, its size file_kib.
//     - CacheHit: those files with their answers already in a SignatureCache: what a second process of a
//       known image costs the worker (the file opened, its stamp read, the cache found).
//     - FirstSampleBurst: every distinct image of the running processes this user can open, checked once
//       each into an empty cache, as the worker's first samples check them. files is how many, and
//       per_file the time for each; embedded, catalog and other how they came out. The file count is this
//       machine's: compare per_file, not the total, across machines.

#if defined(_WIN32)

#include "Domain/SamplingConfig.h"
#include "Platform/ImageSignature.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsImageSignature.h"
#include "Platform/Windows/WindowsSignatureCheck.h"
#include "Platform/Windows/WindowsSignatureWorker.h"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <tlhelp32.h>

namespace
{

namespace Sig = Platform::WindowsSignature;
using Platform::ImageSignature;
using Platform::SignatureCheck;

constexpr auto DRAIN_TIMEOUT = std::chrono::seconds{30};
constexpr int WARMUP_MAX_SAMPLES = 1000;
constexpr std::size_t PATH_POOL_SIZE = std::size_t{64} * 1024;                          // far more than any path table holds
constexpr std::int64_t DEFAULT_MAX_PATHS = Domain::Sampling::SIGNATURE_CACHE_MAX_FILES; // the product's bound
constexpr std::int64_t SMALL_MAX_PATHS = 256;
constexpr auto CACHE_MAX_FILES = static_cast<std::size_t>(Domain::Sampling::SIGNATURE_CACHE_MAX_FILES);
constexpr std::int64_t REAL_FILE_ITERATIONS = 20;
constexpr double BYTES_PER_KIB = 1024.0;

// ============================================================================
// The worker, with a fake check
// ============================================================================

/// Distinct image paths, shaped like real ones, for the simulated processes; generated once.
[[nodiscard]] const std::vector<std::string>& pathPool()
{
    static const std::vector<std::string> pool = []
    {
        constexpr std::size_t VENDORS = 997;
        std::vector<std::string> paths;
        paths.reserve(PATH_POOL_SIZE);
        for (std::size_t i = 0; i < PATH_POOL_SIZE; ++i)
        {
            paths.push_back(std::format(R"(C:\Program Files\Vendor {:04}\bin\Image_{:06}.exe)", i % VENDORS, i));
        }
        return paths;
    }();
    return pool;
}

/// A check that touches no file: every image opens with the same stamp (with a file ID, so answers are
/// cached) and is signed.
[[nodiscard]] Sig::SignatureFunctions fakeFunctions()
{
    return {
        .errorMessage = nullptr,
        .openImage = [](const std::string& /*path*/) -> std::optional<Sig::OpenedImage>
        {
            return Sig::OpenedImage{
                .file = nullptr,
                .stamp = Sig::FileStamp{.fileId = Sig::FileId{.volumeSerial = 1, .id = {1}}, .changeTime = 1, .size = 1, .usn = 1},
            };
        },
        .closeImage = [](void* /*file*/) {},
        .verifyEmbeddedSignature = [](const std::string& /*path*/, void* /*file*/)
        { return Sig::TrustCheck{.result = 0, .signer = "Fake Signer"}; },
        .verifyCatalogSignature = [](const std::string& /*path*/, void* /*file*/) -> std::optional<Sig::TrustCheck>
        { return std::nullopt; },
    };
}

/// Holds the held check (heldFunctions()) until set; cleared while a Worker_Saturated runs.
[[nodiscard]] std::atomic<bool>& checkReleased()
{
    static std::atomic<bool> released{true};
    return released;
}

/// Opens the held check when it goes out of scope, so the worker's destructor can join its thread.
struct CheckReleaser
{
    CheckReleaser() = default;
    CheckReleaser(const CheckReleaser&) = delete;
    CheckReleaser& operator=(const CheckReleaser&) = delete;
    CheckReleaser(CheckReleaser&&) = delete;
    CheckReleaser& operator=(CheckReleaser&&) = delete;
    ~CheckReleaser()
    {
        checkReleased().store(true);
        checkReleased().notify_all();
    }
};

/// fakeFunctions(), but the check waits until checkReleased() is set: the worker holds its first path, and
/// everything requested after it stays queued.
[[nodiscard]] Sig::SignatureFunctions heldFunctions()
{
    Sig::SignatureFunctions fns = fakeFunctions();
    fns.verifyEmbeddedSignature = [](const std::string& /*path*/, void* /*file*/)
    {
        checkReleased().wait(false);
        return Sig::TrustCheck{.result = 0, .signer = "Fake Signer"};
    };
    return fns;
}

/// The probe's side of the worker (WindowsProcessProbe::updateSignature()), for a fixed number of processes.
class SimulatedProbe
{
  public:
    SimulatedProbe(std::size_t processes, std::size_t maxPaths, const Sig::SignatureFunctions& fns = fakeFunctions())
        : m_Worker(fns, Sig::WorkerOptions{.pause = std::chrono::milliseconds{0}, .maxCachedFiles = maxPaths, .maxPaths = maxPaths}),
          m_Slots(processes)
    {
        for (Slot& slot : m_Slots)
        {
            slot.path = takePath();
        }
    }

    /// @p count paths no process holds (nor ever will: they aren't in pathPool()) requested straight away,
    /// as processes come and gone would have.
    void requestOthers(std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            m_Worker.request(std::format(R"(C:\Program Files\Gone\Image_{:06}.exe)", i));
        }
    }

    [[nodiscard]] std::size_t pathCount() const
    {
        return m_Worker.pathCount();
    }

    /// One sample's calls: each process whose signature isn't in yet asks for it.
    void sample()
    {
        const auto& pool = pathPool();
        m_Worker.beginSample();
        for (Slot& slot : m_Slots)
        {
            if (slot.signature.has_value())
            {
                continue;
            }
            const std::string& path = pool[slot.path];
            if (!slot.requested)
            {
                m_Worker.request(path);
                slot.requested = true;
            }
            slot.signature = m_Worker.find(path);
        }
    }

    /// @p count processes replaced by processes of images not seen for a long while (in turn, oldest first).
    void churn(std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            m_Slots[m_NextSlot] = Slot{.path = takePath(), .signature = std::nullopt, .requested = false};
            m_NextSlot = (m_NextSlot + 1) % m_Slots.size();
        }
    }

    /// Waits for the worker to check everything queued. False if it didn't in time.
    [[nodiscard]] bool drain()
    {
        return m_Worker.waitUntilIdle(DRAIN_TIMEOUT);
    }

    [[nodiscard]] std::size_t unanswered() const
    {
        std::size_t count = 0;
        for (const Slot& slot : m_Slots)
        {
            count += slot.signature.has_value() ? 0U : 1U;
        }
        return count;
    }

    /// Samples (and drains) until every process is answered; the samples it took, or nullopt if it never was.
    [[nodiscard]] std::optional<int> sampleUntilAnswered()
    {
        for (int samples = 1; samples <= WARMUP_MAX_SAMPLES; ++samples)
        {
            sample();
            if (!drain())
            {
                return std::nullopt;
            }
            if (unanswered() == 0)
            {
                return samples;
            }
        }
        return std::nullopt;
    }

  private:
    struct Slot
    {
        std::size_t path = 0; ///< Into pathPool()
        std::optional<SignatureCheck> signature;
        bool requested = false;
    };

    [[nodiscard]] std::size_t takePath()
    {
        const std::size_t path = m_NextPath;
        m_NextPath = (m_NextPath + 1) % PATH_POOL_SIZE;
        return path;
    }

    Sig::SignatureWorker m_Worker;
    std::vector<Slot> m_Slots;
    std::size_t m_NextSlot = 0;
    std::size_t m_NextPath = 0;
};

void BM_WindowsSignature_Worker_Churn(benchmark::State& state)
{
    const auto processes = static_cast<std::size_t>(state.range(0));
    const auto churnPercent = static_cast<std::size_t>(state.range(1));
    const auto maxPaths = static_cast<std::size_t>(state.range(2));
    const std::size_t newPaths = processes * churnPercent / 100;
    SimulatedProbe probe(processes, maxPaths);
    if (!probe.sampleUntilAnswered().has_value())
    {
        state.SkipWithError("The worker never answered every process");
        return;
    }
    for (auto _ : state)
    {
        probe.churn(newPaths);
        probe.sample();
        if (!probe.drain())
        {
            state.SkipWithError("The worker didn't drain its queue");
            return;
        }
    }
    probe.sample(); // untimed: the last sample's answers taken, as the next sample would take them
    state.counters["new_paths"] = benchmark::Counter(static_cast<double>(newPaths));
    state.counters["per_new_path"] =
        benchmark::Counter(static_cast<double>(newPaths), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["unanswered"] = benchmark::Counter(static_cast<double>(probe.unanswered()));
}
BENCHMARK(BM_WindowsSignature_Worker_Churn)
    ->ArgNames({"paths", "churn_pct", "max_paths"})
    ->ArgsProduct({{300, 3000}, {1, 10, 100}, {DEFAULT_MAX_PATHS, SMALL_MAX_PATHS}})
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

void BM_WindowsSignature_Worker_FirstSample(benchmark::State& state)
{
    const auto processes = static_cast<std::size_t>(state.range(0));
    const auto maxPaths = static_cast<std::size_t>(state.range(1));
    int samples = 0;
    for (auto _ : state)
    {
        SimulatedProbe probe(processes, maxPaths);
        const std::optional<int> taken = probe.sampleUntilAnswered();
        if (!taken.has_value())
        {
            state.SkipWithError("The worker never answered every process");
            return;
        }
        samples = *taken;
    }
    state.counters["samples"] = benchmark::Counter(static_cast<double>(samples));
    state.counters["per_path"] =
        benchmark::Counter(static_cast<double>(processes), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_WindowsSignature_Worker_FirstSample)
    ->ArgNames({"paths", "max_paths"})
    ->ArgsProduct({{300, 3000}, {DEFAULT_MAX_PATHS, SMALL_MAX_PATHS}})
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

void BM_WindowsSignature_Worker_Saturated(benchmark::State& state)
{
    const auto processes = static_cast<std::size_t>(state.range(0));
    const auto maxPaths = static_cast<std::size_t>(state.range(1));
    checkReleased().store(false);
    SimulatedProbe probe(processes, maxPaths, heldFunctions());
    const CheckReleaser releaser; // destroyed first: the held check ends before the worker is joined
    probe.requestOthers(maxPaths);
    if (probe.pathCount() != maxPaths)
    {
        state.SkipWithError("The path table didn't fill with queued paths");
        return;
    }
    for (auto _ : state)
    {
        probe.churn(processes);
        probe.sample();
    }
    if (probe.pathCount() != maxPaths)
    {
        state.SkipWithError("The path table admitted a path while full of queued ones");
        return;
    }
    state.counters["new_paths"] = benchmark::Counter(static_cast<double>(processes));
    state.counters["per_new_path"] =
        benchmark::Counter(static_cast<double>(processes), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_WindowsSignature_Worker_Saturated)
    ->ArgNames({"paths", "max_paths"})
    ->ArgsProduct({{30, 300}, {DEFAULT_MAX_PATHS, SMALL_MAX_PATHS}})
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// ============================================================================
// The real check (environment-dependent)
// ============================================================================

/// A System32 file whose signature checkSignature() found to be of the kind wanted.
struct SystemFile
{
    std::string path; ///< UTF-8, as the worker takes it
    std::string name;
    std::uint64_t bytes = 0;
};

[[nodiscard]] std::wstring systemDirectory()
{
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    return (length == 0 || length >= buffer.size()) ? std::wstring{} : std::wstring(buffer.data(), length);
}

/// The first of @p candidates (System32 file names) that checkSignature() says is @p wanted; nullopt when
/// none is. The checks it makes are also the warm-up: the first one in the process is the slow one.
[[nodiscard]] std::optional<SystemFile> findSystemFile(std::initializer_list<std::wstring_view> candidates, ImageSignature wanted)
{
    const std::wstring directory = systemDirectory();
    if (directory.empty())
    {
        return std::nullopt;
    }
    const Sig::SignatureFunctions fns = Platform::Windows::systemSignatureFunctions();
    for (const std::wstring_view candidate : candidates)
    {
        const std::wstring wide = directory + L"\\" + std::wstring(candidate);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &attributes) == 0)
        {
            continue;
        }
        constexpr unsigned HIGH_SHIFT = 32;
        const std::uint64_t bytes = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << HIGH_SHIFT) | attributes.nFileSizeLow;
        std::string path = Platform::WinString::wideToUtf8(wide);
        if (Sig::checkSignature(fns, nullptr, path, Sig::pathKey(path)).signature == wanted)
        {
            return SystemFile{.path = std::move(path), .name = Platform::WinString::wideToUtf8(candidate), .bytes = bytes};
        }
    }
    return std::nullopt;
}

[[nodiscard]] const std::optional<SystemFile>& catalogFile()
{
    static const std::optional<SystemFile> file = findSystemFile({L"notepad.exe", L"cmd.exe", L"kernel32.dll"}, ImageSignature::Catalog);
    return file;
}

[[nodiscard]] const std::optional<SystemFile>& embeddedFile()
{
    static const std::optional<SystemFile> file = findSystemFile({L"vcruntime140.dll", L"msvcp140.dll"}, ImageSignature::Embedded);
    return file;
}

void benchmarkVerify(benchmark::State& state, const std::optional<SystemFile>& file, ImageSignature wanted)
{
    if (!file.has_value())
    {
        state.SkipWithMessage("No System32 file with this kind of signature on this machine");
        return;
    }
    const Sig::SignatureFunctions fns = Platform::Windows::systemSignatureFunctions();
    const std::string key = Sig::pathKey(file->path);
    for (auto _ : state)
    {
        SignatureCheck check = Sig::checkSignature(fns, nullptr, file->path, key);
        if (check.signature != wanted)
        {
            state.SkipWithError("The file's signature check changed its answer");
            return;
        }
        benchmark::DoNotOptimize(check);
    }
    state.SetLabel(file->name);
    state.counters["file_kib"] = benchmark::Counter(static_cast<double>(file->bytes) / BYTES_PER_KIB);
}

void BM_WindowsSignature_VerifyCatalog(benchmark::State& state)
{
    benchmarkVerify(state, catalogFile(), ImageSignature::Catalog);
}
BENCHMARK(BM_WindowsSignature_VerifyCatalog)->Iterations(REAL_FILE_ITERATIONS)->Unit(benchmark::kMillisecond);

void BM_WindowsSignature_VerifyEmbedded(benchmark::State& state)
{
    benchmarkVerify(state, embeddedFile(), ImageSignature::Embedded);
}
BENCHMARK(BM_WindowsSignature_VerifyEmbedded)->Iterations(REAL_FILE_ITERATIONS)->Unit(benchmark::kMillisecond);

void BM_WindowsSignature_CacheHit(benchmark::State& state)
{
    std::vector<const SystemFile*> files;
    for (const std::optional<SystemFile>* file : {&catalogFile(), &embeddedFile()})
    {
        if (file->has_value())
        {
            files.push_back(&**file);
        }
    }
    if (files.empty())
    {
        state.SkipWithMessage("No signed System32 file found on this machine");
        return;
    }
    const Sig::SignatureFunctions fns = Platform::Windows::systemSignatureFunctions();
    Sig::SignatureCache cache(CACHE_MAX_FILES);
    std::vector<std::string> keys;
    for (const SystemFile* file : files)
    {
        keys.push_back(Sig::pathKey(file->path));
        (void) Sig::checkSignature(fns, &cache, file->path, keys.back());
    }
    if (cache.size() != files.size())
    {
        state.SkipWithMessage("The file system gives these files no file ID, so their answers aren't cached");
        return;
    }
    for (auto _ : state)
    {
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            SignatureCheck check = Sig::checkSignature(fns, &cache, files[i]->path, keys[i]);
            benchmark::DoNotOptimize(check);
        }
    }
    state.counters["files"] = benchmark::Counter(static_cast<double>(files.size()));
    state.counters["per_file"] =
        benchmark::Counter(static_cast<double>(files.size()), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_WindowsSignature_CacheHit)->Unit(benchmark::kMicrosecond);

/// The image path of @p pid (UTF-8), or empty when this user can't open it or read it.
[[nodiscard]] std::string imagePathOf(DWORD pid)
{
    const Platform::Windows::UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid())
    {
        return {};
    }
    constexpr DWORD LONG_PATH_CHARS = 32768;
    std::wstring buffer(LONG_PATH_CHARS, L'\0');
    DWORD size = LONG_PATH_CHARS;
    if (QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size) == 0)
    {
        return {};
    }
    buffer.resize(size);
    return Platform::WinString::wideToUtf8(buffer);
}

/// Every distinct image path (by pathKey()) of the processes running now that this user can open.
[[nodiscard]] std::vector<std::string> runningImagePaths()
{
    std::vector<std::string> paths;
    const Platform::Windows::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
    {
        return paths;
    }
    std::unordered_set<std::string> seen;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != 0; more = Process32NextW(snapshot.get(), &entry))
    {
        if (entry.th32ProcessID == 0)
        {
            continue;
        }
        std::string path = imagePathOf(entry.th32ProcessID);
        if (!path.empty() && seen.insert(Sig::pathKey(path)).second)
        {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

void BM_WindowsSignature_FirstSampleBurst(benchmark::State& state)
{
    (void) catalogFile(); // the warm-up: the process's first check loads wintrust and the catalog database
    const std::vector<std::string> paths = runningImagePaths();
    if (paths.empty())
    {
        state.SkipWithError("Could not read any running process's image path");
        return;
    }
    std::vector<std::string> keys;
    keys.reserve(paths.size());
    for (const std::string& path : paths)
    {
        keys.push_back(Sig::pathKey(path));
    }
    const Sig::SignatureFunctions fns = Platform::Windows::systemSignatureFunctions();
    std::size_t embedded = 0;
    std::size_t catalog = 0;
    for (auto _ : state)
    {
        Sig::SignatureCache cache(CACHE_MAX_FILES);
        embedded = 0;
        catalog = 0;
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            const SignatureCheck check = Sig::checkSignature(fns, &cache, paths[i], keys[i]);
            embedded += check.signature == ImageSignature::Embedded ? 1U : 0U;
            catalog += check.signature == ImageSignature::Catalog ? 1U : 0U;
        }
        benchmark::DoNotOptimize(cache);
    }
    state.counters["files"] = benchmark::Counter(static_cast<double>(paths.size()));
    state.counters["per_file"] =
        benchmark::Counter(static_cast<double>(paths.size()), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["embedded"] = benchmark::Counter(static_cast<double>(embedded));
    state.counters["catalog"] = benchmark::Counter(static_cast<double>(catalog));
    state.counters["other"] = benchmark::Counter(static_cast<double>(paths.size() - embedded - catalog));
}
BENCHMARK(BM_WindowsSignature_FirstSampleBurst)->Iterations(1)->Unit(benchmark::kMillisecond);

} // namespace

#endif // _WIN32
