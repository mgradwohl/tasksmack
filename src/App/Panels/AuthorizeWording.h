#pragma once

// The words of the buttons that redo a refused action or read as an administrator (#2308, #2309), through
// Windows' helper, TaskSmackHelper.exe (#2550). Its UAC prompt asks an administrator to approve it, as "Run as
// administrator" does, often with no password at all, so the buttons say "as administrator". Linux has no
// helper since #3580, so nothing there draws these buttons: one set of words, Windows' (#3810).

#include "UI/IconsFontAwesome6.h"

// NOLINTBEGIN(cppcoreguidelines-macro-usage) - string literals the labels and tooltips are joined from
#define TASKSMACK_AUTHORIZE_RETRY "Retry as administrator"
#define TASKSMACK_AUTHORIZE_READ "Read as administrator"
#define TASKSMACK_AUTHORIZE_SEARCH "Search them as administrator too"
#define TASKSMACK_AUTHORIZE_ASKS "Asks Windows for an administrator's approval (a UAC prompt)"
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
/// Find handles' button beside the processes a search couldn't read (#3067, #3357), which searches them again.
inline constexpr const char* SEARCH_LABEL = ICON_FA_LOCK " " TASKSMACK_AUTHORIZE_SEARCH;

/// The tooltips: what the button asks for, then what it does.
inline constexpr const char* RETRY_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then does it as the administrator";
inline constexpr const char* READ_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then reads it as the administrator";
inline constexpr const char* SEARCH_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then searches the processes this run may not read";
inline constexpr const char* PRIORITY_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then sets the priority as the administrator";
inline constexpr const char* IO_PRIORITY_TOOLTIP = TASKSMACK_AUTHORIZE_ASKS ", then sets the I/O priority as the administrator";

/// The access choice's words (#2728): the startup notice's and the status-bar chip's, Windows', as the helper's are.
/// The constants above are its retry and read words, and tests/App/test_AccessChoice.cpp holds them equal.
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

} // namespace App::AuthorizeWording

#undef TASKSMACK_AUTHORIZE_RETRY
#undef TASKSMACK_AUTHORIZE_READ
#undef TASKSMACK_AUTHORIZE_SEARCH
#undef TASKSMACK_AUTHORIZE_ASKS
