#pragma once

// The words of the buttons that redo a refused action or read as an administrator (#2308, #2309), in
// each platform's own terms (#2550). On Linux pkexec asks for a password, and polkit's own dialog says
// "Authentication is required", so the buttons say "Authenticate". On Windows a UAC prompt asks an
// administrator to approve the helper, as "Run as administrator" does, often with no password at all,
// so the buttons say "as administrator" instead.

#include "App/AccessChoice.h"
#include "UI/IconsFontAwesome6.h"

// NOLINTBEGIN(cppcoreguidelines-macro-usage) - string literals the labels and tooltips are joined from
#ifdef _WIN32
#define TASKSMACK_AUTHORIZE_RETRY "Retry as administrator"
#define TASKSMACK_AUTHORIZE_READ "Read as administrator"
#define TASKSMACK_AUTHORIZE_ASKS "Asks Windows for an administrator's approval (a UAC prompt)"
#else
#define TASKSMACK_AUTHORIZE_RETRY "Authenticate and retry"
#define TASKSMACK_AUTHORIZE_READ "Authenticate to read"
#define TASKSMACK_AUTHORIZE_ASKS "Asks for an administrator's password"
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace App::AuthorizeWording
{

/// The words of the button beside a refused action, which does it again as an administrator.
inline constexpr const char* RETRY_TEXT = TASKSMACK_AUTHORIZE_RETRY;
/// The words of the button beside a refused read, which reads again as an administrator.
inline constexpr const char* READ_TEXT = TASKSMACK_AUTHORIZE_READ;

/// The buttons' labels: the lock, then the words.
inline constexpr const char* RETRY_LABEL = ICON_FA_LOCK "  " TASKSMACK_AUTHORIZE_RETRY;
inline constexpr const char* READ_LABEL = ICON_FA_LOCK "  " TASKSMACK_AUTHORIZE_READ;

/// The tooltips: what the button asks for, then what it does.
inline constexpr const char* RETRY_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then does it as the administrator";
inline constexpr const char* READ_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then reads it as the administrator";
inline constexpr const char* PRIORITY_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then sets the priority as the administrator";
inline constexpr const char* IO_PRIORITY_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then sets the I/O priority as the administrator";

/// One platform's words for the access choice (#2728), as data, so one test run checks every platform's
/// (tests/App/test_AccessChoice.cpp); the constants above are the host's, and the test holds them equal.
struct PlatformWording
{
    const char* launch;        ///< The notice's Administrator option
    const char* retry;         ///< Beside a refused action
    const char* read;          ///< Beside a refused read
    const char* switchSession; ///< The status-bar chip's switch, mid-session
    const char* promptTooltip; ///< Names the prompt the choice raises
};

inline constexpr PlatformWording WINDOWS_WORDING{
    .launch = "Run as administrator",
    .retry = "Retry as administrator",
    .read = "Read as administrator",
    .switchSession = "Restart as administrator...",
    .promptTooltip = "Windows asks for approval (UAC)",
};

inline constexpr PlatformWording LINUX_WORDING{
    .launch = "Run with administrator access",
    .retry = "Authenticate and retry",
    .read = "Authenticate to read",
    .switchSession = "Authenticate for this session...",
    .promptTooltip = "You'll be asked for an administrator's password (polkit)",
};

[[nodiscard]] constexpr const PlatformWording& wordingFor(AccessChoice::AccessPlatform platform) noexcept
{
    return platform == AccessChoice::AccessPlatform::Windows ? WINDOWS_WORDING : LINUX_WORDING;
}

} // namespace App::AuthorizeWording

#undef TASKSMACK_AUTHORIZE_RETRY
#undef TASKSMACK_AUTHORIZE_READ
#undef TASKSMACK_AUTHORIZE_ASKS
