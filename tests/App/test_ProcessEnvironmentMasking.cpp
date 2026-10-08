/// @file test_ProcessEnvironmentMasking.cpp
/// @brief Which environment variable names Process Details masks (#179): the rule table, case
/// insensitivity, and the false positives it must avoid (PWD, KEYBOARD, XDG_SESSION_TYPE).

#include "App/Panels/ProcessEnvironmentMasking.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace App::Detail
{
namespace
{

struct NameCase
{
    std::string_view name;
    bool secret;
};

void expectAll(std::span<const NameCase> cases)
{
    for (const NameCase& c : cases)
    {
        SCOPED_TRACE(std::string(c.name));
        EXPECT_EQ(isSecretEnvironmentName(c.name), c.secret);
    }
}

TEST(ProcessEnvironmentMaskingTest, MasksSecretLookingNames)
{
    constexpr std::array<NameCase, 33> CASES{{
        {.name = "MY_API_TOKEN", .secret = true},
        {.name = "GITHUB_TOKEN", .secret = true},
        {.name = "GH_TOKEN", .secret = true},
        {.name = "AWS_SECRET_ACCESS_KEY", .secret = true},
        {.name = "AWS_ACCESS_KEY_ID", .secret = true},
        {.name = "AWS_SESSION_TOKEN", .secret = true},
        {.name = "OPENAI_API_KEY", .secret = true},
        {.name = "APIKEY", .secret = true},
        {.name = "STRIPE_KEY", .secret = true},
        {.name = "SSH_KEYS", .secret = true},
        {.name = "SSHKEY", .secret = true},
        {.name = "DB_PASSWORD", .secret = true},
        {.name = "MYSQL_PWD", .secret = true},
        {.name = "PGPASSWORD", .secret = true},
        {.name = "SMTP_PASSWD", .secret = true},
        {.name = "DB_PASS", .secret = true},
        {.name = "PASSCODE", .secret = true},
        {.name = "GPG_PASSPHRASE", .secret = true},
        {.name = "BASIC_AUTH", .secret = true},
        {.name = "AUTHORIZATION", .secret = true},
        {.name = "AUTHTOKEN", .secret = true},
        {.name = "GOOGLE_APPLICATION_CREDENTIALS", .secret = true},
        {.name = "BROWSER_COOKIE", .secret = true},
        {.name = "SESSION", .secret = true},
        {.name = "USER_SESSION_ID", .secret = true},
        {.name = "PRIVATE_KEY_PEM", .secret = true},
        {.name = "TLS_CERT", .secret = true},
        {.name = "CLIENT_CERTIFICATE", .secret = true},
        {.name = "SENTRY_DSN", .secret = true},
        {.name = "DATABASE_CONNECTION_STRING", .secret = true}, // matched with the separators removed
        {.name = "SQLCONNSTR_main", .secret = true},
        {.name = "DATABASE_URL", .secret = true},
        {.name = "BEARER", .secret = true},
    }};
    expectAll(CASES);
}

TEST(ProcessEnvironmentMaskingTest, IsCaseInsensitive)
{
    constexpr std::array<NameCase, 5> CASES{{
        {.name = "my_api_token", .secret = true},
        {.name = "Api_Key", .secret = true},
        {.name = "npm_config_password", .secret = true},
        {.name = "pwd", .secret = false},
        {.name = "xdg_session_type", .secret = false},
    }};
    expectAll(CASES);
}

TEST(ProcessEnvironmentMaskingTest, AvoidsObviousFalsePositives)
{
    constexpr std::array<NameCase, 22> CASES{{
        {.name = "PWD", .secret = false},
        {.name = "OLDPWD", .secret = false},
        {.name = "KEYBOARD", .secret = false},
        {.name = "XKB_DEFAULT_KEYMAP", .secret = false},
        {.name = "GNOME_KEYRING_CONTROL", .secret = false},
        {.name = "XDG_SESSION_TYPE", .secret = false},
        {.name = "XDG_SESSION_CLASS", .secret = false},
        {.name = "XDG_SESSION_DESKTOP", .secret = false},
        {.name = "XDG_SESSION_ID", .secret = false},
        {.name = "DESKTOP_SESSION", .secret = false},
        {.name = "DBUS_SESSION_BUS_ADDRESS", .secret = false},
        {.name = "SESSION_MANAGER", .secret = false},
        {.name = "SSH_AUTH_SOCK", .secret = false},
        {.name = "GIT_AUTHOR_NAME", .secret = false},
        {.name = "GIT_AUTHOR_EMAIL", .secret = false},
        {.name = "XAUTHORITY", .secret = false},
        {.name = "HOME", .secret = false},
        {.name = "PATH", .secret = false},
        {.name = "LANG", .secret = false},
        {.name = "DISPLAY", .secret = false},
        {.name = "FOO", .secret = false},
        {.name = "", .secret = false},
    }};
    expectAll(CASES);
}

TEST(ProcessEnvironmentMaskingTest, ListedNamesAreUpperCaseAndDistinct)
{
    // The rule upper-cases the name and compares against these as they stand.
    const auto allUpper = [](std::string_view s)
    {
        return std::ranges::none_of(s, [](char c) { return c >= 'a' && c <= 'z'; });
    };
    for (const std::string_view name : NEVER_SECRET_NAMES)
    {
        SCOPED_TRACE(std::string(name));
        EXPECT_TRUE(allUpper(name));
        EXPECT_FALSE(isSecretEnvironmentName(name));
    }
    for (const std::string_view word : SECRET_WORDS)
    {
        SCOPED_TRACE(std::string(word));
        EXPECT_TRUE(allUpper(word));
        EXPECT_TRUE(isSecretEnvironmentName(word) || std::ranges::find(NEVER_SECRET_NAMES, word) != NEVER_SECRET_NAMES.end());
    }
    for (const std::string_view secret : SECRET_SUBSTRINGS)
    {
        SCOPED_TRACE(std::string(secret));
        EXPECT_TRUE(allUpper(secret));
        EXPECT_TRUE(isSecretEnvironmentName(std::string("X_") + std::string(secret) + "_Y"));
    }
}

} // namespace
} // namespace App::Detail
