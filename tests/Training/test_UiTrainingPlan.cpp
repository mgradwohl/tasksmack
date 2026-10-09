/// @file test_UiTrainingPlan.cpp
/// @brief The PGO UI training plan (#880): the workload weights, how a run's frames are split between
/// them, and the driver's command line. The driver itself is run by the UiTrainingSmoke* ctest tests.

#include "Training/UiTrainingPlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using Training::allocateFrames;
using Training::Options;
using Training::parseArguments;
using Training::Probes;
using Training::Segment;
using Training::Tab;
using Training::Workload;
using Training::WORKLOADS;

[[nodiscard]] int totalFrames(const std::vector<Segment>& segments)
{
    int total = 0;
    for (const Segment& segment : segments)
    {
        total += segment.frames;
    }
    return total;
}

/// The options @p result parsed, or (with a test failure) the defaults when it parsed none.
[[nodiscard]] Options optionsOf(const Training::ParseResult& result)
{
    if (!result.options.has_value())
    {
        ADD_FAILURE() << "did not parse: " << result.error;
        return Options{};
    }
    return *result.options;
}

[[nodiscard]] Training::ParseResult parse(std::initializer_list<std::string_view> args)
{
    const std::vector<std::string_view> list(args);
    return parseArguments(list);
}

TEST(UiTrainingPlanTest, WeightsArePercentages)
{
    int sum = 0;
    for (const Workload& workload : WORKLOADS)
    {
        EXPECT_GT(workload.weight, 0) << workload.name;
        sum += workload.weight;
    }
    EXPECT_EQ(sum, 100);
}

TEST(UiTrainingPlanTest, EveryMainTabIsTrained)
{
    constexpr std::array ALL_TABS{Tab::SystemOverview, Tab::Processes, Tab::ProcessDetails, Tab::Services, Tab::Startup, Tab::SystemInfo};
    for (const Tab tab : ALL_TABS)
    {
        EXPECT_TRUE(std::ranges::any_of(WORKLOADS, [tab](const Workload& w) { return w.tab == tab; })) << Training::tabId(tab);
    }
}

TEST(UiTrainingPlanTest, TabIdsAreTheShellsRegisteredIds)
{
    EXPECT_EQ(Training::tabId(Tab::SystemOverview), "SystemOverview");
    EXPECT_EQ(Training::tabId(Tab::Processes), "Processes");
    EXPECT_EQ(Training::tabId(Tab::ProcessDetails), "ProcessDetails");
    EXPECT_EQ(Training::tabId(Tab::Services), "Services");
    EXPECT_EQ(Training::tabId(Tab::Startup), "Startup");
    EXPECT_EQ(Training::tabId(Tab::SystemInfo), "SystemInfo");
}

TEST(UiTrainingPlanTest, ByWeightWhenTheTotalDividesEvenly)
{
    const std::vector<Segment> segments = allocateFrames(1000, WORKLOADS);
    ASSERT_EQ(segments.size(), WORKLOADS.size());
    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        EXPECT_EQ(segments[i].workload, &WORKLOADS[i]);
        EXPECT_EQ(segments[i].frames, WORKLOADS[i].weight * 10) << WORKLOADS[i].name;
    }
}

TEST(UiTrainingPlanTest, LeftoverFramesGoToTheLargestRemainders)
{
    const std::array<Workload, 3> workloads{
        {
            {.name = "a", .tab = Tab::SystemOverview, .weight = 1},
            {.name = "b", .tab = Tab::Processes, .weight = 1},
            {.name = "c", .tab = Tab::Services, .weight = 1},
        },
    };
    // 10 / 3: 3 each and one left over, to the first on the tie.
    const std::vector<Segment> segments = allocateFrames(10, workloads);
    ASSERT_EQ(segments.size(), 3U);
    EXPECT_EQ(segments[0].frames, 4);
    EXPECT_EQ(segments[1].frames, 3);
    EXPECT_EQ(segments[2].frames, 3);

    const std::array<Workload, 2> uneven{
        {
            {.name = "small", .tab = Tab::SystemOverview, .weight = 1},
            {.name = "large", .tab = Tab::Processes, .weight = 2},
        },
    };
    // 4 / 3: floors 1 and 2 (remainders 1 and 2), so the leftover frame goes to "large".
    const std::vector<Segment> unevenSegments = allocateFrames(4, uneven);
    ASSERT_EQ(unevenSegments.size(), 2U);
    EXPECT_EQ(unevenSegments[0].frames, 1);
    EXPECT_EQ(unevenSegments[1].frames, 3);
}

TEST(UiTrainingPlanTest, FramesAlwaysAddUpToTheTotal)
{
    for (const int total : {1, 7, 9, 99, 101, 1800, 12345})
    {
        EXPECT_EQ(totalFrames(allocateFrames(total, WORKLOADS)), total) << total;
    }
}

TEST(UiTrainingPlanTest, AShareThatRoundsToZeroGetsNoSegment)
{
    const std::vector<Segment> segments = allocateFrames(1, WORKLOADS);
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_EQ(segments[0].workload->name, "overview"); // The largest weight takes the only frame
}

TEST(UiTrainingPlanTest, NothingToAllocate)
{
    EXPECT_TRUE(allocateFrames(0, WORKLOADS).empty());
    EXPECT_TRUE(allocateFrames(-5, WORKLOADS).empty());
    EXPECT_TRUE(allocateFrames(100, std::span<const Workload>{}).empty());
    const std::array<Workload, 1> unweighted{{{.name = "zero", .tab = Tab::Startup, .weight = 0}}};
    EXPECT_TRUE(allocateFrames(100, unweighted).empty());
}

TEST(UiTrainingPlanTest, DefaultsWithNoArguments)
{
    const Training::ParseResult result = parse({});
    ASSERT_TRUE(result.options.has_value()) << result.error;
    const Options options = optionsOf(result);
    EXPECT_EQ(options.probes, Probes::Real);
    EXPECT_EQ(options.syntheticSpec, Training::DEFAULT_SYNTHETIC_SPEC);
    EXPECT_EQ(options.frames, Training::DEFAULT_FRAMES);
    EXPECT_EQ(options.fps, Training::DEFAULT_FPS);
    EXPECT_FALSE(options.listOnly);
    EXPECT_FALSE(options.help);
}

TEST(UiTrainingPlanTest, ParsesEveryOptionInBothForms)
{
    const Training::ParseResult spaced = parse({"--probes", "synthetic", "--frames", "120", "--fps", "0", "--list"});
    ASSERT_TRUE(spaced.options.has_value()) << spaced.error;
    EXPECT_EQ(optionsOf(spaced).probes, Probes::Synthetic);
    EXPECT_EQ(optionsOf(spaced).frames, 120);
    EXPECT_EQ(optionsOf(spaced).fps, 0);
    EXPECT_TRUE(optionsOf(spaced).listOnly);

    const Training::ParseResult joined = parse({"--synthetic=processes=300,history=60", "--frames=5", "--fps=30"});
    ASSERT_TRUE(joined.options.has_value()) << joined.error;
    EXPECT_EQ(optionsOf(joined).probes, Probes::Synthetic); // --synthetic implies it
    EXPECT_EQ(optionsOf(joined).syntheticSpec, "processes=300,history=60");
    EXPECT_EQ(optionsOf(joined).frames, 5);
    EXPECT_EQ(optionsOf(joined).fps, 30);

    const Training::ParseResult back = parse({"--synthetic", "processes=10", "--probes", "real"});
    ASSERT_TRUE(back.options.has_value()) << back.error;
    EXPECT_EQ(optionsOf(back).probes, Probes::Real); // The last word wins

    const Training::ParseResult help = parse({"--help"});
    ASSERT_TRUE(help.options.has_value());
    EXPECT_TRUE(optionsOf(help).help);
}

TEST(UiTrainingPlanTest, RejectsBadArguments)
{
    for (const auto& args : std::vector<std::vector<std::string_view>>{
             {"--frames", "0"},
             {"--frames", "-3"},
             {"--frames", "12x"},
             {"--frames", "1000001"},
             {"--fps", "-1"},
             {"--fps", "1001"},
             {"--probes", "fake"},
             {"--synthetic="},
             {"--frames"},
             {"--kill-everything"},
             {"frames"},
         })
    {
        const Training::ParseResult result = parseArguments(args);
        EXPECT_FALSE(result.options.has_value()) << args.front();
        EXPECT_FALSE(result.error.empty()) << args.front();
    }
}

TEST(UiTrainingPlanTest, DescribesThePlan)
{
    Options options;
    options.probes = Probes::Synthetic;
    options.frames = 200;
    const std::string text = Training::describePlan(options, allocateFrames(options.frames, WORKLOADS));
    EXPECT_NE(text.find("200 frames"), std::string::npos) << text;
    EXPECT_NE(text.find("probes synthetic (processes=5000,history=full)"), std::string::npos) << text;
    EXPECT_NE(text.find("overview"), std::string::npos) << text;
    EXPECT_NE(text.find("50 frames"), std::string::npos) << text; // overview: 25% of 200
    EXPECT_NE(Training::usage().find("--synthetic SPEC"), std::string::npos);
}

} // namespace
