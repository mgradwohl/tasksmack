#include "Training/UiTrainingPlan.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Training
{

namespace
{

/// A whole number in [minimum, maximum], or nullopt.
[[nodiscard]] std::optional<int> parseInt(std::string_view text, int minimum, int maximum) noexcept
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum)
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string_view probesName(Probes probes) noexcept
{
    return probes == Probes::Synthetic ? "synthetic" : "real";
}

} // namespace

std::string_view tabId(Tab tab) noexcept
{
    switch (tab)
    {
    case Tab::SystemOverview:
        return "SystemOverview";
    case Tab::Processes:
        return "Processes";
    case Tab::ProcessDetails:
        return "ProcessDetails";
    case Tab::Services:
        return "Services";
    case Tab::Startup:
        return "Startup";
    case Tab::SystemInfo:
        return "SystemInfo";
    }
    return "SystemOverview";
}

std::vector<Segment> allocateFrames(int totalFrames, std::span<const Workload> workloads)
{
    std::int64_t weightSum = 0;
    for (const Workload& workload : workloads)
    {
        weightSum += std::max(workload.weight, 0);
    }
    if (totalFrames <= 0 || weightSum <= 0)
    {
        return {};
    }

    struct Share
    {
        int frames = 0;
        std::int64_t remainder = 0;
    };
    std::vector<Share> shares(workloads.size());
    std::int64_t assigned = 0;
    for (std::size_t i = 0; i < workloads.size(); ++i)
    {
        const std::int64_t scaled = static_cast<std::int64_t>(totalFrames) * std::max(workloads[i].weight, 0);
        shares[i].frames = static_cast<int>(scaled / weightSum);
        shares[i].remainder = scaled % weightSum;
        assigned += shares[i].frames;
    }

    // The frames the floors left over, one each to the largest remainders; the earlier on a tie.
    std::vector<std::size_t> order(workloads.size());
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&shares](std::size_t a, std::size_t b) { return shares[a].remainder > shares[b].remainder; });
    for (std::size_t k = 0; assigned < totalFrames && k < order.size(); ++k, ++assigned)
    {
        ++shares[order[k]].frames;
    }

    std::vector<Segment> segments;
    segments.reserve(workloads.size());
    for (std::size_t i = 0; i < workloads.size(); ++i)
    {
        if (shares[i].frames > 0)
        {
            segments.push_back({.workload = &workloads[i], .frames = shares[i].frames});
        }
    }
    return segments;
}

ParseResult parseArguments(std::span<const std::string_view> args)
{
    Options options;
    ParseResult result;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        std::string_view key = args[i];
        std::optional<std::string_view> inlineValue;
        if (const std::size_t eq = key.find('='); key.starts_with("--") && eq != std::string_view::npos)
        {
            inlineValue = key.substr(eq + 1);
            key = key.substr(0, eq);
        }

        if (key == "--help" || key == "-h")
        {
            options.help = true;
            continue;
        }
        if (key == "--list")
        {
            options.listOnly = true;
            continue;
        }

        if (key != "--probes" && key != "--synthetic" && key != "--frames" && key != "--fps")
        {
            result.error = std::format("unknown argument '{}'", args[i]);
            return result;
        }
        std::string_view value;
        if (inlineValue.has_value())
        {
            value = *inlineValue;
        }
        else if (i + 1 < args.size())
        {
            value = args[++i];
        }
        else
        {
            result.error = std::format("{} needs a value", key);
            return result;
        }

        if (key == "--probes")
        {
            if (value == "real")
            {
                options.probes = Probes::Real;
            }
            else if (value == "synthetic")
            {
                options.probes = Probes::Synthetic;
            }
            else
            {
                result.error = std::format("--probes is real or synthetic, not '{}'", value);
                return result;
            }
        }
        else if (key == "--synthetic")
        {
            if (value.empty())
            {
                result.error = "--synthetic needs a scenario, e.g. processes=5000,history=full";
                return result;
            }
            options.probes = Probes::Synthetic;
            options.syntheticSpec = std::string(value);
        }
        else if (key == "--frames")
        {
            const std::optional<int> frames = parseInt(value, 1, MAX_FRAMES);
            if (!frames.has_value())
            {
                result.error = std::format("--frames is a whole number from 1 to {}, not '{}'", MAX_FRAMES, value);
                return result;
            }
            options.frames = *frames;
        }
        else // --fps
        {
            const std::optional<int> fps = parseInt(value, 0, MAX_FPS);
            if (!fps.has_value())
            {
                result.error = std::format("--fps is a whole number from 0 to {}, not '{}'", MAX_FPS, value);
                return result;
            }
            options.fps = *fps;
        }
    }
    result.options = std::move(options);
    return result;
}

std::string usage()
{
    return std::format("Usage: TaskSmackUiTraining [--probes real|synthetic] [--synthetic SPEC] [--frames N] [--fps N] [--list]\n"
                       "\n"
                       "Renders TaskSmack's real panels headless (no window, no GPU) for PGO training (#880):\n"
                       "every main tab, by the weights in src/Training/UiTrainingPlan.h, then exits.\n"
                       "No process, service or startup action is ever taken: the driver gives ImGui no input.\n"
                       "\n"
                       "  --probes real|synthetic  the real probes, or the synthetic large-UI machine (default real)\n"
                       "  --synthetic SPEC         the synthetic machine, TASKSMACK_SYNTHETIC syntax; implies synthetic\n"
                       "                           (default {})\n"
                       "  --frames N               frames in the whole run, 1-{} (default {})\n"
                       "  --fps N                  pace frames to N per second; 0 = unpaced (default {})\n"
                       "  --list                   print the plan and exit\n",
                       DEFAULT_SYNTHETIC_SPEC,
                       MAX_FRAMES,
                       DEFAULT_FRAMES,
                       DEFAULT_FPS);
}

std::string describePlan(const Options& options, std::span<const Segment> segments)
{
    std::string text = std::format("UI training: {} frames, {}, probes {}",
                                   options.frames,
                                   options.fps > 0 ? std::format("{} fps", options.fps) : std::string("unpaced"),
                                   probesName(options.probes));
    if (options.probes == Probes::Synthetic)
    {
        text += std::format(" ({})", options.syntheticSpec);
    }
    text += '\n';
    for (const Segment& segment : segments)
    {
        text += std::format("  {:<18} {:>7} frames  (weight {})\n", segment.workload->name, segment.frames, segment.workload->weight);
    }
    return text;
}

} // namespace Training
