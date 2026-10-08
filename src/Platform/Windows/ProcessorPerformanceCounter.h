#pragma once

// PDH's "% Processor Performance" for WindowsSystemProbe's current CPU clock (#1184). In its own header,
// with the PDH functions in an injectable table, so tests can drive the probe with fabricated readings
// and failures instead of whatever this machine's clock is doing.

#include "Platform/Windows/WindowsProcAddress.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>     // PDH types only: pdh.dll is loaded at run time
#include <pdhmsg.h>  // PDH_CSTATUS_VALID_DATA, PDH_CSTATUS_NEW_DATA
// clang-format on

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace Platform
{

/// The PDH functions ProcessorPerformanceCounter calls: pdh.dll's exports, or a test's fakes.
struct PdhFunctions
{
    using OpenQueryFn = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
    using AddEnglishCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
    using CollectQueryDataFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using GetFormattedCounterValueFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
    using CloseQueryFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);

    OpenQueryFn openQuery = nullptr;
    AddEnglishCounterFn addEnglishCounter = nullptr;
    CollectQueryDataFn collectQueryData = nullptr;
    GetFormattedCounterValueFn getFormattedCounterValue = nullptr;
    CloseQueryFn closeQuery = nullptr;

    [[nodiscard]] bool complete() const noexcept
    {
        return openQuery != nullptr && addEnglishCounter != nullptr && collectQueryData != nullptr && getFormattedCounterValue != nullptr &&
               closeQuery != nullptr;
    }
};

/// Calls FreeLibrary on a module LoadLibrary returned.
struct ModuleDeleter
{
    void operator()(HMODULE module) const noexcept
    {
        FreeLibrary(module);
    }
};

/// A loaded module, freed when it goes out of scope, so no exit path can leak the reference.
using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;

/// The counter path, in English so it is found whatever the system's display language.
inline constexpr const wchar_t* PROCESSOR_PERFORMANCE_COUNTER_PATH = LR"(\Processor Information(_Total)\% Processor Performance)";

/// PDH's "\Processor Information(_Total)\% Processor Performance": the processors' average speed as a
/// percentage of the base clock, over the time since the previous read, as Task Manager's "Speed" uses
/// it (#1184). pdh.dll is loaded at run time, as PDHGPUProbe loads it, so the probe still works where
/// it is missing.
class ProcessorPerformanceCounter
{
  public:
    /// The counter from pdh.dll, or null when pdh.dll, its exports, or the counter are unavailable.
    [[nodiscard]] static std::unique_ptr<ProcessorPerformanceCounter> open()
    {
        UniqueModule module(LoadLibraryExW(L"pdh.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
        if (module == nullptr)
        {
            spdlog::debug("WindowsSystemProbe: pdh.dll unavailable; reporting the base CPU clock");
            return nullptr;
        }
        HMODULE pdh = module.get();
        const PdhFunctions functions{
            .openQuery = Windows::getProcAddress<PdhFunctions::OpenQueryFn>(pdh, "PdhOpenQueryW"),
            .addEnglishCounter = Windows::getProcAddress<PdhFunctions::AddEnglishCounterFn>(pdh, "PdhAddEnglishCounterW"),
            .collectQueryData = Windows::getProcAddress<PdhFunctions::CollectQueryDataFn>(pdh, "PdhCollectQueryData"),
            .getFormattedCounterValue =
                Windows::getProcAddress<PdhFunctions::GetFormattedCounterValueFn>(pdh, "PdhGetFormattedCounterValue"),
            .closeQuery = Windows::getProcAddress<PdhFunctions::CloseQueryFn>(pdh, "PdhCloseQuery"),
        };
        return open(std::move(module), functions);
    }

    /// The counter through `functions` alone (a test's fakes), or null as open(module, functions) is.
    [[nodiscard]] static std::unique_ptr<ProcessorPerformanceCounter> open(const PdhFunctions& functions)
    {
        return open(UniqueModule{}, functions);
    }

    /// The counter through `functions`, which come from `module` (or from no module: a test's fakes), or
    /// null when one is missing or the query or counter can't be opened. `module` is owned from here on:
    /// it is freed on every path that doesn't return the counter, a throw included, and with the counter
    /// otherwise -- after its query is closed.
    [[nodiscard]] static std::unique_ptr<ProcessorPerformanceCounter> open(UniqueModule module, const PdhFunctions& functions)
    {
        if (!functions.complete())
        {
            spdlog::debug("WindowsSystemProbe: pdh.dll lacks an export; reporting the base CPU clock");
            return nullptr;
        }
        auto counter = std::unique_ptr<ProcessorPerformanceCounter>(new ProcessorPerformanceCounter(functions));
        // From here the counter owns the module and, once opened, the query: its destructor closes the
        // query and then frees the module, whichever way this function is left.
        counter->m_Module = std::move(module);
        if (functions.openQuery(nullptr, 0, &counter->m_Query) != ERROR_SUCCESS)
        {
            counter->m_Query = nullptr;
            spdlog::debug("WindowsSystemProbe: PdhOpenQuery failed; reporting the base CPU clock");
            return nullptr;
        }
        if (functions.addEnglishCounter(counter->m_Query, PROCESSOR_PERFORMANCE_COUNTER_PATH, 0, &counter->m_Counter) != ERROR_SUCCESS)
        {
            spdlog::debug("WindowsSystemProbe: no % Processor Performance counter; reporting the base CPU clock");
            return nullptr;
        }
        // The first collection only primes the rate
        functions.collectQueryData(counter->m_Query);
        return counter;
    }

    ProcessorPerformanceCounter(const ProcessorPerformanceCounter&) = delete;
    ProcessorPerformanceCounter& operator=(const ProcessorPerformanceCounter&) = delete;
    ProcessorPerformanceCounter(ProcessorPerformanceCounter&&) = delete;
    ProcessorPerformanceCounter& operator=(ProcessorPerformanceCounter&&) = delete;

    ~ProcessorPerformanceCounter()
    {
        // The query is closed before m_Module, a member, frees the functions' module
        if (m_Query != nullptr)
        {
            m_Functions.closeQuery(m_Query);
        }
    }

    /// The percentage since the previous read; nullopt on the first read (a rate needs two) or a failed one.
    [[nodiscard]] std::optional<double> read()
    {
        if (m_Functions.collectQueryData(m_Query) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        PDH_FMT_COUNTERVALUE value{};
        DWORD type = 0;
        // NOCAP100: turbo takes the reading past 100; without it PDH caps a percentage at 100
        const PDH_STATUS status = m_Functions.getFormattedCounterValue(m_Counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &type, &value);
        if (status != ERROR_SUCCESS || (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA))
        {
            return std::nullopt;
        }
        // PDH_FMT_DOUBLE asked for the double member of PDH_FMT_COUNTERVALUE's union
        return value.doubleValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
    }

  private:
    explicit ProcessorPerformanceCounter(const PdhFunctions& functions) : m_Functions(functions)
    {}

    PdhFunctions m_Functions;
    UniqueModule m_Module; // Null for injected functions; freed after the destructor closes the query
    PDH_HQUERY m_Query = nullptr;
    PDH_HCOUNTER m_Counter = nullptr;
};

} // namespace Platform
