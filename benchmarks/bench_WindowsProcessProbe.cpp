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
//
// The process count and the snapshot's size are this machine's; they are reported as counters, so
// runs on different machines, or with a different load, are not compared as like for like.

#if defined(_WIN32)

#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcessProbe.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
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

} // namespace

#endif // defined(_WIN32)
