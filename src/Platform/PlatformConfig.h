#pragma once

/// Platform-specific feature detection macros
///
/// This header centralizes conditional compilation macros for platform features.
/// Include this header where you need to check platform capabilities.

// Netlink INET_DIAG support for per-process network monitoring (Linux only)
// Requires inet_diag.h and sock_diag.h kernel headers
#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - required for #if conditional compilation
#define TASKSMACK_HAS_NETLINK_SOCKET_STATS 1
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - required for #if conditional compilation
#define TASKSMACK_HAS_NETLINK_SOCKET_STATS 0
#endif
