#include "UdpCountingStatus.h"

#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace
{

struct Published
{
    std::mutex mutex;
    UdpCountingStatus status;
};

[[nodiscard]] Published& published()
{
    static Published instance;
    return instance;
}

} // namespace

UdpCountingStatus udpCountingStatus()
{
    Published& state = published();
    const std::scoped_lock lock(state.mutex);
    return state.status;
}

void publishUdpCountingStatus(UdpCountingStatus status)
{
    Published& state = published();
    const std::scoped_lock lock(state.mutex);
    state.status = std::move(status);
}

std::string UdpCountingLine::text() const
{
    std::string line;
    line.reserve(lead.size() + detail.size() + end.size());
    line.append(lead).append(detail).append(end);
    return line;
}

UdpCountingLine udpCountingLine(bool on, bool delivering, bool unsupported, std::string_view problem) noexcept
{
    if (on && delivering)
    {
        return {.lead = "Counting UDP.", .detail = {}, .end = {}, .warning = false};
    }
    if (unsupported)
    {
        return {.lead = "This system can't count UDP per process: ", .detail = problem, .end = {}, .warning = true};
    }
    if (!problem.empty())
    {
        // Why it isn't counting, as a sentence: "Needs TaskSmack running as administrator."
        return {.lead = {}, .detail = problem, .end = problem.ends_with('.') ? "" : ".", .warning = false};
    }
    if (on)
    {
        return {.lead = "On: UDP is counted from the first UDP traffic.", .detail = {}, .end = {}, .warning = false};
    }
    return {.lead = "Off: Net Sent and Net Received count TCP only.", .detail = {}, .end = {}, .warning = false};
}

} // namespace App
