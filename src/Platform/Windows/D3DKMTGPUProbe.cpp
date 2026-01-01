#include "D3DKMTGPUProbe.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <unordered_map>
#include <utility>

// clang-format off
// Windows headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Suppress __uuidof extension warning for DXGI
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <dxgi.h>
#pragma clang diagnostic pop

#include <psapi.h>
#include <tlhelp32.h>
// clang-format on

// D3DKMT types and functions (declared locally to avoid WDK dependency)
// These are standard Windows kernel-mode graphics types available via gdi32.dll

// NTSTATUS type (from ntdef.h)
typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS) 0x00000000L)

typedef UINT D3DKMT_HANDLE;

struct D3DKMT_OPENADAPTERFROMLUID
{
    LUID AdapterLuid;
    D3DKMT_HANDLE hAdapter;
};

enum D3DKMT_QUERYSTATISTICS_TYPE
{
    D3DKMT_QUERYSTATISTICS_ADAPTER = 0,
    D3DKMT_QUERYSTATISTICS_PROCESS = 1,
    D3DKMT_QUERYSTATISTICS_PROCESS_ADAPTER = 2,
    D3DKMT_QUERYSTATISTICS_SEGMENT = 3
};

struct D3DKMT_QUERYSTATISTICS_MEMORY
{
    UINT64 BytesAllocated;
    UINT64 BytesReserved;
    UINT64 CommitLimit;
    UINT64 BytesResident;
    UINT64 BytesResidentInSharedMemory;
};

struct D3DKMT_QUERYSTATISTICS
{
    D3DKMT_QUERYSTATISTICS_TYPE Type;
    LUID AdapterLuid;
    HANDLE hProcess;
    union
    {
        struct
        {
            ULONG ProcessId;
        } QueryProcessStatistics;
    };
    union
    {
        struct
        {
            D3DKMT_QUERYSTATISTICS_MEMORY SystemMemory;
        } ProcessStatistics;
    } QueryResult;
};

// Function prototypes (exported from gdi32.dll)
extern "C"
{
    NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID* pAdapter);
    NTSTATUS WINAPI D3DKMTQueryStatistics(D3DKMT_QUERYSTATISTICS* pQueryStats);
}

namespace Platform
{

struct D3DKMTGPUProbe::Impl
{
    struct AdapterInfo
    {
        D3DKMT_HANDLE adapterHandle{};
        LUID adapterLuid{};
        std::string gpuId;
        std::string gpuName;
        bool isIntegrated = false;
    };

    std::vector<AdapterInfo> adapters;
    bool initialized = false;

    bool initialize();
    [[nodiscard]] std::string luidToString(const LUID& luid) const;
    [[nodiscard]] std::vector<std::uint32_t> enumerateProcessIds() const;
};

bool D3DKMTGPUProbe::Impl::initialize()
{
    if (initialized)
    {
        return true;
    }

    // Use DXGI to enumerate adapters and get LUID
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr)
    {
        return false;
    }

    std::uint32_t adapterIndex = 0;
    IDXGIAdapter1* adapter = nullptr;

    while (factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            // Skip software adapters
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                adapter->Release();
                ++adapterIndex;
                continue;
            }

            AdapterInfo info;
            info.adapterLuid = desc.AdapterLuid;
            info.gpuId = luidToString(desc.AdapterLuid);

            // Convert wide string to UTF-8
            int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
            if (sizeNeeded > 0)
            {
                std::string utf8Name(static_cast<std::size_t>(sizeNeeded) - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8Name.data(), sizeNeeded, nullptr, nullptr);
                info.gpuName = std::move(utf8Name);
            }

            // Detect integrated vs discrete
            info.isIntegrated = (desc.DedicatedVideoMemory == 0 || desc.DedicatedVideoMemory < (128ULL * 1024 * 1024));

            // Open adapter using D3DKMT
            D3DKMT_OPENADAPTERFROMLUID openAdapter{};
            openAdapter.AdapterLuid = desc.AdapterLuid;
            auto status = D3DKMTOpenAdapterFromLuid(&openAdapter);
            if (status == STATUS_SUCCESS)
            {
                info.adapterHandle = openAdapter.hAdapter;
                adapters.push_back(std::move(info));
            }
        }

        adapter->Release();
        ++adapterIndex;
    }

    factory->Release();

    initialized = !adapters.empty();
    return initialized;
}

std::string D3DKMTGPUProbe::Impl::luidToString(const LUID& luid) const
{
    char buffer[32]{};
    std::snprintf(
        buffer, sizeof(buffer), "%08lX%08lX", static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
    return {buffer};
}

std::vector<std::uint32_t> D3DKMTGPUProbe::Impl::enumerateProcessIds() const
{
    std::vector<std::uint32_t> pids;

    // Take a snapshot of all processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return pids;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &pe32))
    {
        do
        {
            pids.push_back(pe32.th32ProcessID);
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return pids;
}

// Constructor
D3DKMTGPUProbe::D3DKMTGPUProbe() : m_Impl(std::make_unique<Impl>())
{
}

D3DKMTGPUProbe::~D3DKMTGPUProbe() = default;

std::vector<GPUInfo> D3DKMTGPUProbe::enumerateGPUs()
{
    if (!m_Impl->initialize())
    {
        return {};
    }

    std::vector<GPUInfo> gpus;
    gpus.reserve(m_Impl->adapters.size());

    for (const auto& adapter : m_Impl->adapters)
    {
        GPUInfo info;
        info.id = adapter.gpuId;
        info.name = adapter.gpuName;
        info.isIntegrated = adapter.isIntegrated;
        info.vendor = "Unknown"; // D3DKMT doesn't provide vendor info directly
        info.deviceIndex = static_cast<std::uint32_t>(&adapter - m_Impl->adapters.data());
        gpus.push_back(std::move(info));
    }

    return gpus;
}

std::vector<GPUCounters> D3DKMTGPUProbe::readGPUCounters()
{
    // D3DKMT doesn't provide system-level GPU utilization or clocks
    // This probe is specifically for per-process metrics
    // System metrics should come from DXGI or NVML
    return {};
}

std::vector<ProcessGPUCounters> D3DKMTGPUProbe::readProcessGPUCounters()
{
    if (!m_Impl->initialize())
    {
        return {};
    }

    std::vector<ProcessGPUCounters> allCounters;

    // Enumerate all processes
    auto pids = m_Impl->enumerateProcessIds();

    // Query each process for GPU usage across all adapters
    for (auto pid : pids)
    {
        // Skip system process
        if (pid == 0 || pid == 4)
        {
            continue;
        }

        for (const auto& adapter : m_Impl->adapters)
        {
            // Query process statistics for this adapter
            D3DKMT_QUERYSTATISTICS queryStats{};
            queryStats.Type = D3DKMT_QUERYSTATISTICS_PROCESS;
            queryStats.AdapterLuid = adapter.adapterLuid;
            queryStats.hProcess = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(pid));
            queryStats.QueryProcessStatistics.ProcessId = pid;

            auto status = D3DKMTQueryStatistics(&queryStats);
            if (status != STATUS_SUCCESS)
            {
                continue;
            }

            // Check if process has any GPU activity
            auto& processStats = queryStats.QueryResult.ProcessStatistics.SystemMemory;
            std::uint64_t totalMemory = processStats.BytesAllocated + processStats.BytesReserved;

            if (totalMemory == 0)
            {
                continue; // Process doesn't use this GPU
            }

            ProcessGPUCounters counters;
            counters.pid = static_cast<std::int32_t>(pid);
            counters.gpuId = adapter.gpuId;
            counters.gpuMemoryBytes = totalMemory;

            // D3DKMT doesn't provide real-time GPU utilization percentage directly
            // It provides running time per node (engine), but converting to % requires
            // tracking deltas over time, which is done in the Domain layer

            // For now, we report memory usage and engine activity
            // Engine utilization would require tracking node running times

            allCounters.push_back(std::move(counters));
        }
    }

    return allCounters;
}

GPUCapabilities D3DKMTGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    // D3DKMT provides per-process GPU memory and engine usage
    caps.hasPerProcessMetrics = m_Impl->initialized;
    caps.hasEngineUtilization = m_Impl->initialized;
    caps.supportsMultiGPU = m_Impl->initialized;

    // D3DKMT doesn't provide system-level metrics
    caps.hasTemperature = false;
    caps.hasPowerMetrics = false;
    caps.hasClockSpeeds = false;
    caps.hasFanSpeed = false;
    caps.hasPCIeMetrics = false;
    caps.hasEncoderDecoder = false;

    return caps;
}

} // namespace Platform
