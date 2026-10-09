// TaskSmackUiTraining: the headless PGO UI training driver (#880). See UiTrainingDriver.h for what it
// renders and UiTrainingPlan.h for the workloads and their weights; tools/pgo.sh and tools/pgo.ps1 run
// it after the benchmarks, once with the real probes and once with the synthetic large-UI machine.

#include "Training/UiTrainingDriver.h"
#include "Training/UiTrainingPlan.h"

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    try
    {
        const std::span<char*> argSpan(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> args;
        for (const char* arg : argSpan.subspan(1))
        {
            args.emplace_back(arg);
        }
        const Training::ParseResult parsed = Training::parseArguments(args);
        if (!parsed.options.has_value())
        {
            std::print(stderr, "TaskSmackUiTraining: {}\n\n{}", parsed.error, Training::usage());
            return 2;
        }
        const Training::Options& options = *parsed.options;
        if (options.help)
        {
            std::fputs(Training::usage().c_str(), stdout);
            return 0;
        }
        if (options.listOnly)
        {
            std::fputs(Training::describePlan(options, Training::allocateFrames(options.frames, Training::WORKLOADS)).c_str(), stdout);
            return 0;
        }

        spdlog::set_level(spdlog::level::info);
        return Training::run(options);
    }
    catch (const std::exception& e)
    {
        // fputs, which cannot throw: nothing may escape main().
        std::fputs("TaskSmackUiTraining: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
    catch (...)
    {
        std::fputs("TaskSmackUiTraining: unknown error\n", stderr);
        return 1;
    }
}
