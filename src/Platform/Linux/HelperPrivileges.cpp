#include "HelperPrivileges.h"

#include <array>
#include <cerrno>
#include <cstdint>

#include <linux/capability.h>
#include <linux/prctl.h> // PR_* (sys/prctl.h includes it; named for include-cleaner)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Platform::HelperPrivileges
{

namespace
{

/// Every capability number a kernel can know: 0-63. A number past the running kernel's last one fails
/// PR_CAPBSET_DROP with EINVAL, which is skipped.
constexpr unsigned LAST_POSSIBLE_CAPABILITY = 63;
constexpr unsigned BITS_PER_WORD = 32;

struct CapabilitySets
{
    std::uint64_t effective = 0;
    std::uint64_t permitted = 0;
    std::uint64_t inheritable = 0;
};

/// This thread's capability sets: capget(2), version 3 (two 32-bit words per set).
[[nodiscard]] bool readCapabilities(CapabilitySets& sets) noexcept
{
    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    std::array<__user_cap_data_struct, 2> data{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - syscall(2) is variadic; glibc has no capget wrapper
    if (::syscall(SYS_capget, &header, data.data()) != 0)
    {
        return false;
    }
    const auto join = [](std::uint32_t low, std::uint32_t high)
    {
        return std::uint64_t{low} | (std::uint64_t{high} << BITS_PER_WORD);
    };
    sets.effective = join(data[0].effective, data[1].effective);
    sets.permitted = join(data[0].permitted, data[1].permitted);
    sets.inheritable = join(data[0].inheritable, data[1].inheritable);
    return true;
}

/// capset(2): this thread's sets become @p sets.
[[nodiscard]] bool writeCapabilities(const CapabilitySets& sets) noexcept
{
    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    const auto low = [](std::uint64_t mask)
    {
        return static_cast<std::uint32_t>(mask);
    };
    const auto high = [](std::uint64_t mask)
    {
        return static_cast<std::uint32_t>(mask >> BITS_PER_WORD);
    };
    std::array<__user_cap_data_struct, 2> data{{
        {.effective = low(sets.effective), .permitted = low(sets.permitted), .inheritable = low(sets.inheritable)},
        {.effective = high(sets.effective), .permitted = high(sets.permitted), .inheritable = high(sets.inheritable)},
    }};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - syscall(2) is variadic; glibc has no capset wrapper
    return ::syscall(SYS_capset, &header, data.data()) == 0;
}

} // namespace

DropResult dropToKeptCapabilities() noexcept
{
    // First, so that even a failure below leaves nothing an exec could raise.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - prctl(2) is variadic
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0)
    {
        return DropResult::Failed;
    }
    CapabilitySets current;
    if (!readCapabilities(current))
    {
        return DropResult::Failed;
    }
    if (current.permitted == 0 && current.effective == 0 && current.inheritable == 0)
    {
        return DropResult::NotPrivileged;
    }

    // The bounding set first: dropping from it needs CAP_SETPCAP, which goes with the rest below. Without
    // CAP_SETPCAP (a container that withholds it) the bounding set stays; no_new_privs and the helper
    // starting no program keep it from mattering.
    const bool canDropBounding = ((current.effective >> CAP_SETPCAP) & 1U) != 0;
    for (unsigned capability = 0; canDropBounding && capability <= LAST_POSSIBLE_CAPABILITY; ++capability)
    {
        if (((KEPT_CAPABILITIES >> capability) & 1U) != 0)
        {
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - prctl(2) is variadic
        if (::prctl(PR_CAPBSET_DROP, static_cast<unsigned long>(capability), 0UL, 0UL, 0UL) != 0 && errno != EINVAL)
        {
            return DropResult::Failed; // EINVAL: a number past this kernel's last capability
        }
    }
    // The ambient set (Linux 4.3+; EINVAL before it, when there's none to clear).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - prctl(2) is variadic
    if (::prctl(PR_CAP_AMBIENT, static_cast<unsigned long>(PR_CAP_AMBIENT_CLEAR_ALL), 0UL, 0UL, 0UL) != 0 && errno != EINVAL)
    {
        return DropResult::Failed;
    }

    const std::uint64_t kept = current.permitted & KEPT_CAPABILITIES;
    if (!writeCapabilities({.effective = kept, .permitted = kept, .inheritable = 0}))
    {
        return DropResult::Failed;
    }
    // Read back: the drop must have happened, not only been asked for.
    CapabilitySets after;
    if (!readCapabilities(after) || after.effective != kept || after.permitted != kept || after.inheritable != 0)
    {
        return DropResult::Failed;
    }
    return DropResult::Dropped;
}

} // namespace Platform::HelperPrivileges
