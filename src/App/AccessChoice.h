#pragma once

// Which platform's access model applies (#2728). On Linux a session is Administrator for its whole life, started
// as root ("TaskSmack (Administrator)" or sudo), or Standard for its whole life: nothing elevates after launch
// (#3580), so the Standard notice only says how to start one (AccessNoticeDialog). Windows' Administrator restarts
// TaskSmack elevated.

#include <cstdint>

namespace App::AccessChoice
{

enum class AccessPlatform : std::uint8_t
{
    Windows,
    Linux,
};

/// The platform this build runs on.
#ifdef _WIN32
inline constexpr AccessPlatform HOST = AccessPlatform::Windows;
#else
inline constexpr AccessPlatform HOST = AccessPlatform::Linux;
#endif

} // namespace App::AccessChoice
