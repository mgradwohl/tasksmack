#pragma once

// The user's own settings, read and written with the user's own permissions (#4802). A capabilities session (#4766:
// not root, with the FAQ's setcap) holds CAP_DAC_READ_SEARCH, which reads past every file and folder permission, and
// may hold CAP_DAC_OVERRIDE, which writes past them. Its settings folder comes from the user's environment and home
// (XDG_CONFIG_HOME, HOME, TASKSMACK_CONFIG_DIR), so a path there, or a symlink in the user's home, could lead those
// capabilities into another user's settings (their VirusTotal key among them).
//
// While a UserPermissionsOnly lives, this thread's effective set holds neither capability, so the kernel checks an open
// as it would for the user, folders included, with no window between a check and the open. The destructor raises them
// again from the permitted set. Capabilities are per thread: no other thread is touched. A root session is left alone:
// it reaches the user's settings through its held folder, as the user (#4670), and needs its capabilities there.

#include "Platform/Linux/CapabilityBits.h"

#include <array>
#include <cstdint>

#include <linux/capability.h>
#include <sys/syscall.h> // NOLINT(misc-include-cleaner): SYS_capget and SYS_capset, which include-cleaner has no mapping for
#include <unistd.h>

namespace Platform
{

class UserPermissionsOnly
{
  public:
    /// Lowers CAP_DAC_READ_SEARCH and CAP_DAC_OVERRIDE from this thread's effective set when a non-root process holds
    /// either there. ok() is false only when they were held and couldn't be lowered: the caller then opens nothing.
    UserPermissionsOnly() noexcept
    {
        if (::geteuid() == 0)
        {
            return;
        }
        Data data{};
        if (!get(data))
        {
            m_Ok = false; // unknown: assume the worst
            return;
        }
        m_Lowered = data[0].effective & MASK;
        if (m_Lowered == 0)
        {
            return;
        }
        data[0].effective &= ~MASK;
        if (!set(data))
        {
            m_Lowered = 0;
            m_Ok = false;
        }
    }

    ~UserPermissionsOnly()
    {
        if (m_Lowered == 0)
        {
            return;
        }
        if (Data data{}; get(data))
        {
            data[0].effective |= m_Lowered; // still in the permitted set: the thread may raise them again
            static_cast<void>(set(data));
        }
    }

    UserPermissionsOnly(const UserPermissionsOnly&) = delete;
    UserPermissionsOnly& operator=(const UserPermissionsOnly&) = delete;
    UserPermissionsOnly(UserPermissionsOnly&&) = delete;
    UserPermissionsOnly& operator=(UserPermissionsOnly&&) = delete;

    /// False when the capabilities couldn't be lowered (or read): open nothing then.
    [[nodiscard]] bool ok() const noexcept
    {
        return m_Ok;
    }

    /// Whether this guard lowered anything (a capabilities session); for tests and the debug log.
    [[nodiscard]] bool lowered() const noexcept
    {
        return m_Lowered != 0;
    }

  private:
    using Data = std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3>;
    // Both bits are below 32, in the first word.
    static constexpr std::uint32_t MASK =
        (std::uint32_t{1} << CapabilityBits::DAC_OVERRIDE) | (std::uint32_t{1} << CapabilityBits::DAC_READ_SEARCH);
    static_assert(CapabilityBits::DAC_OVERRIDE < 32 && CapabilityBits::DAC_READ_SEARCH < 32);

    static bool get(Data& data) noexcept
    {
        __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,misc-include-cleaner) - syscall(2) is variadic; SYS_capget as above
        return ::syscall(SYS_capget, &header, data.data()) == 0;
    }

    static bool set(Data& data) noexcept
    {
        __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,misc-include-cleaner) - syscall(2) is variadic; SYS_capset as above
        return ::syscall(SYS_capset, &header, data.data()) == 0;
    }

    std::uint32_t m_Lowered = 0;
    bool m_Ok = true;
};

} // namespace Platform
