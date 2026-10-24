#pragma once

// Standard or Administrator (#2728): whether the startup notice shows, and what its Administrator
// option offers, as a pure function of the platform, the process's elevation, the Windows token, the
// Linux helper and the notice setting, so every combination is unit-tested (tests/App/test_AccessChoice.cpp).
// The maintainer's decisions it encodes:
//   - TaskSmack always starts as Standard: Standard is the default in every case, and nothing here
//     starts TaskSmack as Administrator on its own.
//   - The notice never shows once TaskSmack is elevated (Windows) or root (Linux), or when the user
//     turned it off ("Don't show this again"; Settings turns it back on).
//   - Linux's Administrator is the session-held helper (pkexec, #2307), so without the helper (the
//     tar.gz, a source build) the option is disabled and says how to get it, with today's remedy.
//   - Windows' Administrator restarts TaskSmack elevated, which needs no helper; a standard user's
//     UAC prompt asks for an administrator's name and password, and the option says so.

#include <cstdint>
#include <string_view>

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

/// The Windows user's token: one that UAC can elevate (an administrator's split token), or a standard
/// user's, whose elevation runs as another account. Ignored on Linux.
enum class TokenType : std::uint8_t
{
    AdminCapable,
    StandardUser,
};

struct NoticeInputs
{
    AccessPlatform platform = HOST;
    bool elevated = false; ///< Windows: an elevated token; Linux: running as root
    TokenType token = TokenType::AdminCapable;
    bool helperInstalled = false; ///< Linux: tasksmack-helper and its polkit policy are installed
    bool noticeEnabled = true;    ///< The user's "show at startup" setting
};

enum class Choice : std::uint8_t
{
    Standard,
    Administrator,
};

/// The notice's Administrator option.
struct AdministratorOption
{
    bool enabled = false;
    /// Shown under the option: why it is disabled and what enables it, or what choosing it will need.
    /// Empty when there is nothing to add.
    std::string_view note;
};

struct NoticeDecision
{
    bool show = false;
    Choice defaultChoice = Choice::Standard; ///< Standard, always (decision 2)
    AdministratorOption administrator;
};

/// Linux without the helper: the packages install it; until then, today's remedy.
inline constexpr std::string_view NO_HELPER_NOTE =
    "Install the .deb or .rpm package to enable Administrator. Without it, run TaskSmack with sudo, or grant it the "
    "capabilities it needs (CAP_SYS_PTRACE, CAP_DAC_READ_SEARCH).";
/// A Windows standard user: UAC asks for another account's credentials.
inline constexpr std::string_view STANDARD_USER_NOTE = "An administrator's name and password will be needed.";

[[nodiscard]] constexpr NoticeDecision decideNotice(const NoticeInputs& inputs) noexcept
{
    NoticeDecision decision;
    decision.show = !inputs.elevated && inputs.noticeEnabled;
    if (inputs.platform == AccessPlatform::Linux)
    {
        decision.administrator = inputs.helperInstalled ? AdministratorOption{.enabled = true, .note = {}}
                                                        : AdministratorOption{.enabled = false, .note = NO_HELPER_NOTE};
    }
    else
    {
        decision.administrator =
            AdministratorOption{.enabled = true, .note = inputs.token == TokenType::StandardUser ? STANDARD_USER_NOTE : std::string_view{}};
    }
    return decision;
}

} // namespace App::AccessChoice
