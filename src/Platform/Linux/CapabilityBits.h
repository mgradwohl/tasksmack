#pragma once

/// @file CapabilityBits.h
/// @brief The capabilities(7) bit numbers TaskSmack uses, in one place (#3770): RootCapabilities' kept set and
/// ProcPrivileges' checks of what this process may read and do both take them from here, so the two can't drift.
/// Spelled out, not taken from <linux/capability.h>, so this builds against kernel headers older than CAP_PERFMON and
/// CAP_BPF.

namespace Platform::CapabilityBits
{

inline constexpr unsigned CHOWN = 0;
inline constexpr unsigned DAC_OVERRIDE = 1;
inline constexpr unsigned DAC_READ_SEARCH = 2;
inline constexpr unsigned FOWNER = 3;
inline constexpr unsigned KILL = 5;
inline constexpr unsigned SETGID = 6;
inline constexpr unsigned SETUID = 7;
inline constexpr unsigned SYS_PTRACE = 19;
inline constexpr unsigned SYS_ADMIN = 21;
inline constexpr unsigned SYS_NICE = 23;
inline constexpr unsigned SYS_RESOURCE = 24;
inline constexpr unsigned SYSLOG = 34;
inline constexpr unsigned PERFMON = 38;
inline constexpr unsigned BPF = 39;

} // namespace Platform::CapabilityBits
