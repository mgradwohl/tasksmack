#include "UdpCountingStatus.h"

#include <mutex>
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

} // namespace App
