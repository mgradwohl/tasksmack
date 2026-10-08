#pragma once

// Which environment variables Process Details masks (#179).
//
// Every variable is listed, but the value of one whose *name* looks like it holds a secret is shown
// as a row of bullets until its row's reveal button is pressed. The rule is a pure function of the
// name, so it is tested on its own (tests/App/test_ProcessEnvironmentMasking.cpp) and documented in
// the user guide ("Environment variables"). It errs toward masking: a masked path costs a click,
// an unmasked token is a leak.
//
// The name is upper-cased, then (first match wins):
//   1. NEVER_SECRET_NAMES: exact names known to hold no secret (PWD the working directory, the
//      XDG/D-Bus session descriptors, the ssh-agent socket path) are shown.
//   2. SECRET_SUBSTRINGS: a name containing any of these anywhere, once its separators are removed
//      (DATABASE_CONNECTION_STRING -> DATABASECONNECTIONSTRING), is masked
//      (TOKEN, SECRET, PASSWORD, PASSWD, PASSPHRASE, CREDENTIAL, COOKIE, PRIVATE, APIKEY, ...).
//   3. Words: the name is split into words at every character that is not a letter or digit
//      (MY_API_KEY -> MY, API, KEY), and it is masked if a word
//        - is one of SECRET_WORDS (KEY, KEYS, PASS, PWD, PIN, AUTH, SESSION, CERT, DSN, SALT, ...), or
//        - ends in KEY or KEYS (SSHKEY, APIKEYS), or
//        - starts with AUTH but is not AUTHOR(S) (AUTHTOKEN, AUTHORIZATION -- GIT_AUTHOR_NAME is shown), or
//        - starts or ends with PASS, SESSION or CERT (PASSPHRASE, CERTIFICATE, USERSESSION).
//   Words rather than substrings for these, so KEYBOARD, KEYMAP and KEYRING, or XDG_SESSION_TYPE
//   (listed in rule 1), are not caught by KEY or SESSION alone. A word merely ending in PASS (BYPASS)
//   is still masked: erring toward masking.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace App::Detail
{

/// Exact (upper-cased) names that match a secret word below but are known not to hold a secret.
inline constexpr std::array<std::string_view, 12> NEVER_SECRET_NAMES{
    "PWD",                      // the working directory
    "OLDPWD",                   // the previous working directory
    "XDG_SESSION_TYPE",         // x11 / wayland / tty
    "XDG_SESSION_CLASS",        // user / greeter
    "XDG_SESSION_DESKTOP",      // desktop name
    "XDG_SESSION_ID",           // logind session number
    "XDG_SESSION_PATH",         // logind object path
    "DESKTOP_SESSION",          // desktop name
    "DBUS_SESSION_BUS_ADDRESS", // the session bus socket
    "SESSION_MANAGER",          // the X session manager's socket
    "SSH_AUTH_SOCK",            // the ssh-agent socket path (the keys stay in the agent)
    "GPG_AGENT_INFO",           // the gpg-agent socket path
};

/// Masked wherever they appear in the (upper-cased) name, even inside a longer word.
inline constexpr std::array<std::string_view, 15> SECRET_SUBSTRINGS{
    "TOKEN",
    "SECRET",
    "PASSWORD",
    "PASSWD",
    "PASSPHRASE",
    "CREDENTIAL",
    "COOKIE",
    "PRIVATE",
    "APIKEY",
    "ACCESSKEY",
    "SIGNINGKEY",
    "CONNECTIONSTRING",
    "CONNSTR",
    "DATABASEURL", // DATABASE_URL: a connection URL, user:password@host as often as not
    "BEARER",
};

/// Masked when they are a whole word of the name (split at every non-alphanumeric character).
inline constexpr std::array<std::string_view, 14> SECRET_WORDS{
    "KEY",
    "KEYS",
    "PASS",
    "PWD",
    "PIN",
    "AUTH",
    "SESSION",
    "CERT",
    "CERTS",
    "DSN",
    "SALT",
    "OTP",
    "JWT",
    "SID",
};

namespace EnvironmentMaskingDetail
{

[[nodiscard]] inline std::string toUpperAscii(std::string_view text)
{
    std::string upper(text);
    std::ranges::transform(upper, upper.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return upper;
}

[[nodiscard]] inline bool isWordChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

/// Whether one upper-cased word of a name marks it as a secret (rule 3 above).
[[nodiscard]] inline bool isSecretWord(std::string_view word) noexcept
{
    if (std::ranges::find(SECRET_WORDS, word) != SECRET_WORDS.end())
    {
        return true;
    }
    if (word.ends_with("KEY") || word.ends_with("KEYS"))
    {
        return true;
    }
    if (word.starts_with("AUTH") && word != "AUTHOR" && word != "AUTHORS")
    {
        return true;
    }
    constexpr std::array<std::string_view, 3> EDGE_WORDS{"PASS", "SESSION", "CERT"};
    return std::ranges::any_of(EDGE_WORDS, [word](std::string_view edge) { return word.starts_with(edge) || word.ends_with(edge); });
}

} // namespace EnvironmentMaskingDetail

/// Whether the value of the environment variable @p name should be masked until revealed.
/// Case-insensitive; see the rule at the top of this file.
[[nodiscard]] inline bool isSecretEnvironmentName(std::string_view name)
{
    namespace Masking = EnvironmentMaskingDetail;
    const std::string upper = Masking::toUpperAscii(name);
    if (std::ranges::find(NEVER_SECRET_NAMES, std::string_view{upper}) != NEVER_SECRET_NAMES.end())
    {
        return false;
    }
    std::string compact = upper;
    std::erase_if(compact, [](char c) { return !Masking::isWordChar(c); });
    if (std::ranges::any_of(SECRET_SUBSTRINGS, [&compact](std::string_view secret) { return compact.contains(secret); }))
    {
        return true;
    }
    std::string_view rest{upper};
    while (!rest.empty())
    {
        const auto* const wordStart = std::ranges::find_if(rest, Masking::isWordChar);
        rest.remove_prefix(static_cast<std::size_t>(wordStart - rest.begin()));
        const auto* const wordEnd = std::ranges::find_if_not(rest, Masking::isWordChar);
        const auto wordLength = static_cast<std::size_t>(wordEnd - rest.begin());
        if (wordLength > 0 && Masking::isSecretWord(rest.substr(0, wordLength)))
        {
            return true;
        }
        rest.remove_prefix(wordLength);
    }
    return false;
}

} // namespace App::Detail
