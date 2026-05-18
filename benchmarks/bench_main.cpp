// Benchmark entry point with custom main() for global setup.
// Suppresses spdlog output (which goes to stdout by default and would corrupt
// --benchmark_format=json output) then delegates to the standard benchmark runner.

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

int main(int argc, char** argv)
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
