#include "RootCapabilities.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <string>

#include <linux/capability.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Platform::RootCapabilities
{

namespace
{

/// This process's thread count from /proc/self/status, or 0 when it can't be read.
[[nodiscard]] int threadCount()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.starts_with("Threads:"))
        {
            try
            {
                return std::stoi(line.substr(8));
            }
            catch (...) // NOLINT(bugprone-empty-catch): an unreadable count is reported as 0
            {
                return 0;
            }
        }
    }
    return 0;
}

} // namespace

DropResult dropUnused() noexcept
{
    DropResult result;
    result.root = ::geteuid() == 0;
    if (!result.root)
    {
        return result;
    }
    try
    {
        result.threads = threadCount();
    }
    catch (...) // NOLINT(bugprone-empty-catch): the count is only reported
    {}

    // The raw syscalls, in version 3's two 32-bit words, so there's no libcap dependency.
    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> data{};
    if (::syscall(SYS_capget, &header, data.data()) != 0)
    {
        result.error = errno;
        return result;
    }
    const auto join = [](std::uint32_t low, std::uint32_t high)
    {
        return (std::uint64_t{high} << 32U) | low;
    };
    result.before = Sets{
        .effective = join(data[0].effective, data[1].effective),
        .permitted = join(data[0].permitted, data[1].permitted),
        .inheritable = join(data[0].inheritable, data[1].inheritable),
    };
    const Sets wanted = narrowed(result.before);
    const auto split = [](std::uint64_t mask, unsigned word)
    {
        return static_cast<std::uint32_t>(mask >> (32U * word));
    };
    for (unsigned word = 0; word < 2; ++word)
    {
        data[word].effective = split(wanted.effective, word);
        data[word].permitted = split(wanted.permitted, word);
        data[word].inheritable = split(wanted.inheritable, word);
    }
    header = {.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    if (::syscall(SYS_capset, &header, data.data()) != 0)
    {
        result.error = errno;
        result.after = result.before;
        return result;
    }
    result.dropped = true;
    result.after = wanted;
    return result;
}

} // namespace Platform::RootCapabilities
