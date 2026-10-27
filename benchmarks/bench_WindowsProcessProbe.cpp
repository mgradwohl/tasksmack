// Benchmarks for Platform::WindowsProcessProbe::enumerate(), split into its phases (Windows only, #2982)
//
// BM_ProcessProbe_Enumerate (bench_ProcessModel.cpp) times one enumerate() end to end on the real
// clock. Iterations a few milliseconds apart almost never find a detail TTL expired (light 1 s, heavy
// 4 s on a 16 GB+ machine), so it measures the snapshot and its parse and leaves out the per-process
// detail reads a sample at the default 1 s interval makes. These time each phase alone:
//
//  - SnapshotQuery: the bulk NtQuerySystemInformation(SystemProcessInformation) call alone, into a
//    reused buffer, as enumerate() makes it.
//  - ParseWarmCache: enumerate() over one captured snapshot, replayed by a fake query, with the detail
//    caches' clock held still: the record parse, the thread-state tally, the cache lookups and the
//    copies of the cached details, with no process handle opened. SnapshotReplay is the replay's own
//    cost (a copy of the captured bytes), to subtract from it.
//  - EnumerateAtInterval/<ms>: the real enumerate() with the detail caches' clock moved on <ms> per
//    call, as samples that far apart would see it: 0 keeps every cache warm (snapshot + parse), 1000
//    is the default refresh interval (the light details read every sample, the heavy ones every
//    fourth on a 16 GB+ machine), 60000 expires every TTL every call (light and heavy read for every
//    process each sample).
//  - Detail_<read>: each read of the heavy detail pass alone (WindowsProcessDetailReads, the probe's
//    own functions, through its system seams), once over every process this user can open with
//    PROCESS_QUERY_LIMITED_INFORMATION, with the handles opened once beforehand, so each benchmark
//    times one pass of that read alone. The per_process counter is the time per opened process.
//    OpenProcess is the handle a detail refresh opens first (light or heavy). Owner is the whole
//    read without the account name cache (token, its user SID, LookupAccountSidW); OwnerToken is the
//    same with LookupAccountSidW stubbed out, and LookupAccountSid that call alone over the SIDs the
//    token reads found, so the two halves can be told apart. OwnerCached is the read as the probe
//    makes it, with every SID already in its account name cache (#2982). Publisher is the path
//    cache hit getFilePublisher() makes for a path it has seen. ImagePath is the uncached read;
//    ImagePathCached the image path and publisher as the probe takes them on a heavy pass after a
//    process's first, from its own detail cache entry (#2982); its paths_read counter is how many
//    of them read (the rest are tried again every pass, as the probe does). Affinity is the
//    single-group read (GetProcessAffinityMask), and is skipped on a machine with several
//    processor groups.
//
// The process count and the snapshot's size are this machine's; they are reported as counters, so
// runs on different machines, or with a different load, are not compared as like for like.

#if defined(_WIN32)

#include "Platform/CpuAffinity.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <winternl.h>

namespace
{

constexpr LONG STATUS_INFO_LENGTH_MISMATCH_NT = static_cast<LONG>(0xC0000004L);
constexpr std::size_t SNAPSHOT_HEADROOM_BYTES = std::size_t{64} * 1024;
constexpr std::size_t SNAPSHOT_INITIAL_BYTES = std::size_t{256} * 1024;

[[nodiscard]] ULONG bufferLength(const std::vector<std::byte>& buffer)
{
    return buffer.size() > std::numeric_limits<ULONG>::max() ? std::numeric_limits<ULONG>::max() : static_cast<ULONG>(buffer.size());
}

/// One NtQuerySystemInformation(SystemProcessInformation) call into @p buffer, grown and retried while
/// it is too small, as enumerate() does. Returns the bytes written, or nullopt if the call failed.
[[nodiscard]] std::optional<std::size_t> querySnapshot(Platform::Windows::NtQuerySystemInformationFn query, std::vector<std::byte>& buffer)
{
    for (;;)
    {
        ULONG returned = 0;
        const LONG status = query(SystemProcessInformation, buffer.data(), bufferLength(buffer), &returned);
        if (status == STATUS_INFO_LENGTH_MISMATCH_NT)
        {
            buffer.resize(static_cast<std::size_t>(returned) + SNAPSHOT_HEADROOM_BYTES);
            continue;
        }
        if (status < 0)
        {
            return std::nullopt;
        }
        return returned != 0 ? std::min<std::size_t>(returned, buffer.size()) : buffer.size();
    }
}

/// The processes and threads in a snapshot of @p bytes bytes.
struct SnapshotShape
{
    std::size_t processes = 0;
    std::size_t threads = 0;
};

[[nodiscard]] SnapshotShape snapshotShape(const std::vector<std::byte>& buffer, std::size_t bytes)
{
    SnapshotShape shape;
    for (std::size_t offset = 0; offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= bytes;)
    {
        SYSTEM_PROCESS_INFORMATION entry{};
        std::memcpy(&entry, buffer.data() + offset, sizeof(entry));
        ++shape.processes;
        shape.threads += entry.NumberOfThreads;
        if (entry.NextEntryOffset == 0)
        {
            break;
        }
        offset += entry.NextEntryOffset;
    }
    return shape;
}

/// One real snapshot, kept to be replayed: its bytes, and where each entry's image name pointer is,
/// with the offset in the snapshot it points at, so a copy can be pointed at its own names.
struct CapturedSnapshot
{
    std::vector<std::byte> bytes;
    std::vector<std::pair<std::size_t, std::size_t>> namePointers; // {pointer's offset, name's offset}
    SnapshotShape shape;
};

[[nodiscard]] CapturedSnapshot& capturedSnapshot()
{
    static CapturedSnapshot snapshot;
    return snapshot;
}

/// Capture one real snapshot into capturedSnapshot() (once per benchmark binary). False if it failed.
[[nodiscard]] bool captureSnapshot()
{
    auto& snapshot = capturedSnapshot();
    if (!snapshot.bytes.empty())
    {
        return true;
    }
    const auto query = Platform::Windows::ntQuerySystemInformation();
    if (query == nullptr)
    {
        return false;
    }
    std::vector<std::byte> buffer(SNAPSHOT_INITIAL_BYTES);
    const auto bytes = querySnapshot(query, buffer);
    if (!bytes.has_value())
    {
        return false;
    }
    buffer.resize(*bytes);

    const auto base = std::bit_cast<std::uintptr_t>(buffer.data());
    for (std::size_t offset = 0; offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= buffer.size();)
    {
        SYSTEM_PROCESS_INFORMATION entry{};
        std::memcpy(&entry, buffer.data() + offset, sizeof(entry));
        const auto name = std::bit_cast<std::uintptr_t>(entry.ImageName.Buffer);
        if (name >= base && name + entry.ImageName.Length <= base + buffer.size())
        {
            snapshot.namePointers.emplace_back(offset + offsetof(SYSTEM_PROCESS_INFORMATION, ImageName) + offsetof(UNICODE_STRING, Buffer),
                                               name - base);
        }
        if (entry.NextEntryOffset == 0)
        {
            break;
        }
        offset += entry.NextEntryOffset;
    }
    snapshot.shape = snapshotShape(buffer, buffer.size());
    snapshot.bytes = std::move(buffer);
    return true;
}

/// A fake NtQuerySystemInformation that writes the captured snapshot, its image name pointers moved
/// to the caller's buffer, as the kernel would have written it there.
LONG NTAPI replaySnapshot(ULONG /*systemInformationClass*/, PVOID systemInformation, ULONG systemInformationLength, PULONG returnLength)
{
    const auto& snapshot = capturedSnapshot();
    const auto size = static_cast<ULONG>(snapshot.bytes.size());
    if (returnLength != nullptr)
    {
        *returnLength = size;
    }
    if (systemInformationLength < size)
    {
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    auto* const base = static_cast<std::byte*>(systemInformation);
    std::memcpy(base, snapshot.bytes.data(), snapshot.bytes.size());
    for (const auto& [pointerOffset, nameOffset] : snapshot.namePointers)
    {
        // The name's address, written as the pointer's bytes (a uintptr_t is a pointer's size).
        const std::uintptr_t name = std::bit_cast<std::uintptr_t>(base) + nameOffset;
        static_assert(sizeof(name) == sizeof(PWSTR));
        std::memcpy(base + pointerOffset, &name, sizeof(name));
    }
    return 0;
}

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

/// The system's functions, but the detail caches read fakeNow() and no energy meter is opened (no
/// opener thread runs beside the benchmark; enumerate() never reads it).
[[nodiscard]] Platform::WindowsProcessProbeFunctions fakeClockFunctions()
{
    auto functions = Platform::WindowsProcessProbe::systemFunctions();
    functions.system.steadyNow = &readFakeNow;
    functions.energyMeter = std::nullopt;
    return functions;
}

void BM_WindowsProcessProbe_SnapshotQuery(benchmark::State& state)
{
    const auto query = Platform::Windows::ntQuerySystemInformation();
    if (query == nullptr)
    {
        state.SkipWithError("NtQuerySystemInformation unavailable");
        return;
    }
    std::vector<std::byte> buffer(SNAPSHOT_INITIAL_BYTES);
    std::size_t bytes = 0;
    for (auto _ : state)
    {
        const auto written = querySnapshot(query, buffer);
        if (!written.has_value())
        {
            state.SkipWithError("NtQuerySystemInformation(SystemProcessInformation) failed");
            return;
        }
        bytes = *written;
        benchmark::DoNotOptimize(buffer.data());
        benchmark::ClobberMemory();
    }
    const SnapshotShape shape = snapshotShape(buffer, bytes);
    state.counters["processes"] = benchmark::Counter(static_cast<double>(shape.processes));
    state.counters["threads"] = benchmark::Counter(static_cast<double>(shape.threads));
    state.counters["snapshot_kib"] = benchmark::Counter(static_cast<double>(bytes) / 1024.0);
}
BENCHMARK(BM_WindowsProcessProbe_SnapshotQuery)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_SnapshotReplay(benchmark::State& state)
{
    if (!captureSnapshot())
    {
        state.SkipWithError("Could not capture a process snapshot");
        return;
    }
    const auto& snapshot = capturedSnapshot();
    std::vector<std::byte> buffer(snapshot.bytes.size() + SNAPSHOT_HEADROOM_BYTES);
    for (auto _ : state)
    {
        ULONG returned = 0;
        benchmark::DoNotOptimize(replaySnapshot(SystemProcessInformation, buffer.data(), bufferLength(buffer), &returned));
        benchmark::ClobberMemory();
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(snapshot.shape.processes));
    state.counters["snapshot_kib"] = benchmark::Counter(static_cast<double>(snapshot.bytes.size()) / 1024.0);
}
BENCHMARK(BM_WindowsProcessProbe_SnapshotReplay)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_ParseWarmCache(benchmark::State& state)
{
    if (!captureSnapshot())
    {
        state.SkipWithError("Could not capture a process snapshot");
        return;
    }
    auto functions = fakeClockFunctions();
    functions.system.ntQuerySystemInformation = &replaySnapshot;
    Platform::WindowsProcessProbe probe(functions);
    // The first call reads every process's details (opening the captured PIDs, as they are now); the
    // clock then stands still, so no TTL expires and no handle is opened again.
    auto counters = probe.enumerate();
    for (auto _ : state)
    {
        counters = probe.enumerate();
        benchmark::DoNotOptimize(counters.data());
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(counters.size()));
    state.counters["threads"] = benchmark::Counter(static_cast<double>(capturedSnapshot().shape.threads));
}
BENCHMARK(BM_WindowsProcessProbe_ParseWarmCache)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_EnumerateAtInterval(benchmark::State& state)
{
    const auto step = std::chrono::milliseconds(state.range(0));
    Platform::WindowsProcessProbe probe(fakeClockFunctions());
    auto counters = probe.enumerate(); // Every process's details read once, as a first sample does
    for (auto _ : state)
    {
        fakeNow() += step;
        counters = probe.enumerate();
        benchmark::DoNotOptimize(counters.data());
    }
    state.counters["processes"] = benchmark::Counter(static_cast<double>(counters.size()));
}
BENCHMARK(BM_WindowsProcessProbe_EnumerateAtInterval)->ArgName("ms")->Arg(0)->Arg(1000)->Arg(60000)->Unit(benchmark::kMicrosecond);

/// One process this user can open, with what the heavy reads take from earlier ones: its image path
/// (for the publisher and the classification) and whether GetGuiResources reads it (whether the
/// classification counts its USER objects, as getProcessDetails() decides).
struct OpenedProcess
{
    DWORD pid = 0;
    Platform::Windows::UniqueHandle handle;
    std::wstring imagePath;
    std::string imagePathUtf8;
    bool guiReadable = false;
};

/// Every process in the captured snapshot but Idle that OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
/// opens, opened once per benchmark binary and kept open, so each Detail benchmark times its read alone.
[[nodiscard]] std::vector<OpenedProcess>& openedProcesses()
{
    static std::vector<OpenedProcess> processes;
    return processes;
}

/// The user SIDs the owner reads find (one per process whose token opens), for LookupAccountSid.
[[nodiscard]] std::vector<std::vector<std::byte>>& ownerSids()
{
    static std::vector<std::vector<std::byte>> sids;
    return sids;
}

/// The user SID in @p hProcess's token, read as getProcessOwner() reads it; empty if it can't be.
[[nodiscard]] std::vector<std::byte> tokenUserSid(HANDLE hProcess)
{
    Platform::Windows::UniqueHandle token;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, token.put()) == 0)
    {
        return {};
    }
    DWORD length = 0;
    (void) GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    std::vector<std::byte> info(length);
    if (length < sizeof(TOKEN_USER) || GetTokenInformation(token.get(), TokenUser, info.data(), length, &length) == 0)
    {
        return {};
    }
    TOKEN_USER user{};
    std::memcpy(&user, info.data(), sizeof(user));
    if (IsValidSid(user.User.Sid) == 0)
    {
        return {};
    }
    const DWORD sidLength = GetLengthSid(user.User.Sid);
    std::vector<std::byte> sid(sidLength);
    if (CopySid(sidLength, sid.data(), user.User.Sid) == 0)
    {
        return {};
    }
    return sid;
}

/// Open every process in the captured snapshot (once per benchmark binary). False if none opened.
[[nodiscard]] bool openProcesses()
{
    auto& processes = openedProcesses();
    if (!processes.empty())
    {
        return true;
    }
    if (!captureSnapshot())
    {
        return false;
    }
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    const auto& bytes = capturedSnapshot().bytes;
    for (std::size_t offset = 0; offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= bytes.size();)
    {
        SYSTEM_PROCESS_INFORMATION entry{};
        std::memcpy(&entry, bytes.data() + offset, sizeof(entry));
        const auto pid = static_cast<DWORD>(std::bit_cast<std::uintptr_t>(entry.UniqueProcessId));
        if (pid != 0)
        {
            Platform::Windows::UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            if (handle.valid())
            {
                std::wstring imagePath = Platform::WindowsProcessDetailReads::imagePath(functions.process, handle.get());
                std::string imagePathUtf8 = Platform::WinString::wideToUtf8(imagePath);
                SetLastError(0);
                const bool guiReadable = GetGuiResources(handle.get(), GR_GDIOBJECTS) != 0 || GetLastError() == 0;
                OpenedProcess process{.pid = pid,
                                      .handle = std::move(handle),
                                      .imagePath = std::move(imagePath),
                                      .imagePathUtf8 = std::move(imagePathUtf8),
                                      .guiReadable = guiReadable};
                if (auto sid = tokenUserSid(process.handle.get()); !sid.empty())
                {
                    ownerSids().push_back(std::move(sid));
                }
                processes.push_back(std::move(process));
            }
        }
        if (entry.NextEntryOffset == 0)
        {
            break;
        }
        offset += entry.NextEntryOffset;
    }
    return !processes.empty();
}

/// The counters every Detail benchmark reports: how many processes (or SIDs) one pass reads, and the
/// time per one of them.
void reportPerProcess(benchmark::State& state, std::size_t count)
{
    state.counters["processes"] = benchmark::Counter(static_cast<double>(count));
    state.counters["per_process"] =
        benchmark::Counter(static_cast<double>(count), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

/// Runs @p read once for every opened process per iteration; @p read returns what it read.
template<typename Read> void benchmarkEachProcess(benchmark::State& state, Read read)
{
    if (!openProcesses())
    {
        state.SkipWithError("Could not open any process");
        return;
    }
    const auto& processes = openedProcesses();
    for (auto _ : state)
    {
        for (const OpenedProcess& process : processes)
        {
            auto value = read(process);
            benchmark::DoNotOptimize(value);
        }
    }
    reportPerProcess(state, processes.size());
}

void BM_WindowsProcessProbe_Detail_OpenProcess(benchmark::State& state)
{
    benchmarkEachProcess(state,
                         [](const OpenedProcess& process)
                         {
                             const Platform::Windows::UniqueHandle handle(
                                 OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid));
                             return handle.valid();
                         });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_OpenProcess)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_Owner(benchmark::State& state)
{
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    benchmarkEachProcess(state,
                         [&functions](const OpenedProcess& process)
                         { return Platform::WindowsProcessDetailReads::owner(functions.token, process.handle.get()); });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_Owner)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_OwnerCached(benchmark::State& state)
{
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    Platform::AccountNameCache names(std::chrono::hours{1});
    const auto now = std::chrono::steady_clock::now();
    // The probe's steady state: each SID was named on an earlier pass, so only the token is read.
    benchmarkEachProcess(state,
                         [&functions, &names, now](const OpenedProcess& process)
                         { return Platform::WindowsProcessDetailReads::owner(functions.token, process.handle.get(), names, now); });
    state.counters["sids"] = benchmark::Counter(static_cast<double>(names.size()));
}
BENCHMARK(BM_WindowsProcessProbe_Detail_OwnerCached)->Unit(benchmark::kMicrosecond);

/// A LookupAccountSidW that names every SID at once, without asking the LSA.
BOOL WINAPI instantLookupAccountSidW(
    LPCWSTR /*system*/, PSID /*sid*/, LPWSTR name, LPDWORD nameLength, LPWSTR domain, LPDWORD domainLength, PSID_NAME_USE use)
{
    if (name == nullptr || nameLength == nullptr || *nameLength < 2 || domain == nullptr || domainLength == nullptr || *domainLength < 1 ||
        use == nullptr)
    {
        return FALSE;
    }
    name[0] = L'u';
    name[1] = L'\0';
    *nameLength = 1;
    domain[0] = L'\0';
    *domainLength = 0;
    *use = SidTypeUser;
    return TRUE;
}

void BM_WindowsProcessProbe_Detail_OwnerToken(benchmark::State& state)
{
    auto functions = Platform::WindowsProcessProbe::systemFunctions();
    functions.token.lookupAccountSidW = &instantLookupAccountSidW;
    benchmarkEachProcess(state,
                         [&functions](const OpenedProcess& process)
                         { return Platform::WindowsProcessDetailReads::owner(functions.token, process.handle.get()); });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_OwnerToken)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_LookupAccountSid(benchmark::State& state)
{
    if (!openProcesses() || ownerSids().empty())
    {
        state.SkipWithError("No process's token user could be read");
        return;
    }
    auto& sids = ownerSids();
    for (auto _ : state)
    {
        for (auto& sid : sids)
        {
            std::array<WCHAR, 256> name{};
            std::array<WCHAR, 256> domain{};
            auto nameLength = static_cast<DWORD>(name.size());
            auto domainLength = static_cast<DWORD>(domain.size());
            SID_NAME_USE use = SidTypeUnknown;
            benchmark::DoNotOptimize(LookupAccountSidW(nullptr, sid.data(), name.data(), &nameLength, domain.data(), &domainLength, &use));
        }
    }
    reportPerProcess(state, sids.size());
}
BENCHMARK(BM_WindowsProcessProbe_Detail_LookupAccountSid)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_ImagePath(benchmark::State& state)
{
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    benchmarkEachProcess(state,
                         [&functions](const OpenedProcess& process)
                         {
                             return Platform::WinString::wideToUtf8(
                                 Platform::WindowsProcessDetailReads::imagePath(functions.process, process.handle.get()));
                         });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_ImagePath)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_ImagePathCached(benchmark::State& state)
{
    if (!openProcesses())
    {
        state.SkipWithError("Could not open any process");
        return;
    }
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    const auto& processes = openedProcesses();
    // The probe's steady state: each process's image path and publisher were read on an earlier heavy
    // pass, so a later one only takes the publisher from its detail cache entry.
    std::vector<Platform::ProcessImage> images(processes.size());
    std::size_t read = 0;
    for (std::size_t i = 0; i < processes.size(); ++i)
    {
        read += Platform::WindowsProcessDetailReads::image(functions.process, processes[i].handle.get(), images[i]).read ? 1U : 0U;
    }
    for (auto _ : state)
    {
        for (std::size_t i = 0; i < processes.size(); ++i)
        {
            std::string publisher =
                Platform::WindowsProcessDetailReads::image(functions.process, processes[i].handle.get(), images[i]).publisher;
            benchmark::DoNotOptimize(publisher);
        }
    }
    reportPerProcess(state, processes.size());
    state.counters["paths_read"] = benchmark::Counter(static_cast<double>(read));
}
BENCHMARK(BM_WindowsProcessProbe_Detail_ImagePathCached)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_CommandLine(benchmark::State& state)
{
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    benchmarkEachProcess(state,
                         [&functions](const OpenedProcess& process)
                         { return Platform::WindowsProcessDetailReads::commandLine(functions.process, process.handle.get()); });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_CommandLine)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_Publisher(benchmark::State& state)
{
    benchmarkEachProcess(state,
                         [](const OpenedProcess& process) { return Platform::WindowsProcessDetailReads::publisher(process.imagePath); });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_Publisher)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_ProcessType(benchmark::State& state)
{
    const auto functions = Platform::WindowsProcessProbe::systemFunctions();
    benchmarkEachProcess(state,
                         [&functions](const OpenedProcess& process)
                         {
                             return Platform::WindowsProcessDetailReads::processType(functions.process,
                                                                                     process.guiReadable ? process.handle.get() : nullptr,
                                                                                     process.pid,
                                                                                     process.imagePathUtf8);
                         });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_ProcessType)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_PriorityClass(benchmark::State& state)
{
    benchmarkEachProcess(state, [](const OpenedProcess& process) { return GetPriorityClass(process.handle.get()); });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_PriorityClass)->Unit(benchmark::kMicrosecond);

void BM_WindowsProcessProbe_Detail_Affinity(benchmark::State& state)
{
    if (GetMaximumProcessorGroupCount() != 1)
    {
        state.SkipWithError("Several processor groups: the probe reads group affinities, not timed here");
        return;
    }
    benchmarkEachProcess(state,
                         [](const OpenedProcess& process)
                         {
                             DWORD_PTR processMask = 0;
                             DWORD_PTR systemMask = 0;
                             const bool read = GetProcessAffinityMask(process.handle.get(), &processMask, &systemMask) != 0;
                             return read ? Platform::CpuAffinity::fromMask(static_cast<std::uint64_t>(processMask))
                                         : Platform::CpuAffinity{};
                         });
}
BENCHMARK(BM_WindowsProcessProbe_Detail_Affinity)->Unit(benchmark::kMicrosecond);

} // namespace

#endif // defined(_WIN32)
