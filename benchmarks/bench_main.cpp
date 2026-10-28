// Benchmark entry point with custom main() for global setup.
// Suppresses spdlog output (which goes to stdout by default and would corrupt
// --benchmark_format=json output) then delegates to the standard benchmark runner.

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <exception>

#ifdef _WIN32
#include <print>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#endif

namespace
{

// Opt the process out of EcoQoS (#3163). Windows can treat a console process started from a background shell as
// low-QoS and run it throttled, on efficiency cores at a lower clock, which slows a whole run uniformly. A state
// mask of 0 under the EXECUTION_SPEED control bit asks for full speed, whatever the power plan. A failure is
// reported once on stderr (stdout can be the JSON output) and the run goes on: the numbers may then be throttled.
void optOutOfPowerThrottling() noexcept
{
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) == 0)
    {
        const DWORD error = GetLastError();
        try
        {
            std::print(stderr,
                       "TaskSmackBenchmarks: could not opt out of power throttling (EcoQoS), error {}; results may be "
                       "throttled.\n",
                       error);
        }
        catch (...)
        {
            // std::print throws only when it cannot write; the plain message is the best left to try.
            std::fputs("TaskSmackBenchmarks: could not opt out of power throttling (EcoQoS).\n", stderr);
        }
    }
#endif
}

} // namespace

int main(int argc, char** argv)
{
    optOutOfPowerThrottling();

    // spdlog's registry allocates on first use, so an exception can reach here (bugprone-exception-escape).
    try
    {
        // Silence all spdlog output so it does not interleave with JSON benchmark results.
        spdlog::set_level(spdlog::level::off);

        ::benchmark::Initialize(&argc, argv);
        if (::benchmark::ReportUnrecognizedArguments(argc, argv))
        {
            return 1;
        }
        ::benchmark::RunSpecifiedBenchmarks();
        ::benchmark::Shutdown();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fputs("TaskSmackBenchmarks: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
    catch (...)
    {
        std::fputs("TaskSmackBenchmarks: unknown exception\n", stderr);
        return 1;
    }
}
