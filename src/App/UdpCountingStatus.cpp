#include "UdpCountingStatus.h"

#include <atomic>
#include <cstdint>
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
    std::uint64_t version = 1;            // guarded by mutex; bumped when the status changes
    std::atomic<std::uint64_t> latest{1}; // mirror of version for the lock-free unchanged check
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
    if (state.status == status)
    {
        return;
    }
    state.status = std::move(status);
    ++state.version;
    state.latest.store(state.version, std::memory_order_release);
}

bool udpCountingStatusIfChanged(std::uint64_t& seenVersion, UdpCountingStatus& into)
{
    Published& state = published();
    if (state.latest.load(std::memory_order_acquire) == seenVersion)
    {
        return false;
    }
    const std::scoped_lock lock(state.mutex);
    into = state.status;
    seenVersion = state.version;
    return true;
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
