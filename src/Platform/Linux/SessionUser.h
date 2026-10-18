#pragma once

// Whom a TaskSmack run as root for another user should open links and folders as (#2461). Run with sudo
// (as the elevation notice suggests) or pkexec, a browser or file manager started as root either refuses
// to run (Firefox, Chrome, Dolphin) or opens as root on the user's display. So xdg-open is started as the
// user sudo or pkexec names, with that user's session environment. Pure: the environment, the passwd
// lookup and the file checks are passed in, so it is tested without being root.

#include "DetachedSpawn.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace Platform::SessionUser
{

/// What the passwd entry gives of a user.
struct Account
{
    gid_t gid = 0;
    std::string name;
    std::string home;
    std::string shell;
};

/// Variables kept from TaskSmack's own environment: how to reach the user's display and their locale.
/// Everything else (LD_PRELOAD, SUDO_*, root's HOME) is dropped.
inline constexpr std::array<std::string_view, 12> KEPT_VARIABLES{
    "PATH",
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "XAUTHORITY",
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "TZ",
    "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP",
    "XDG_DATA_DIRS",
    "DESKTOP_SESSION",
};

/// The user an elevated TaskSmack was started for and their session environment, or nullopt to start
/// programs as TaskSmack itself: not root (@p effectiveUid isn't 0), root's own session (no SUDO_UID or
/// PKEXEC_UID, or one naming root), or a user the passwd database doesn't know.
/// @p env looks up one of TaskSmack's variables; @p account, a user's passwd entry; @p exists, a path.
[[nodiscard]] inline std::optional<DetachedSpawn::RunAs>
invokingUserSession(uid_t effectiveUid,
                    const std::function<std::optional<std::string>(std::string_view)>& env,
                    const std::function<std::optional<Account>(uid_t)>& account,
                    const std::function<bool(const std::string&)>& exists)
{
    if (effectiveUid != 0)
    {
        return std::nullopt;
    }
    std::optional<std::string> uidText = env("SUDO_UID");
    if (!uidText.has_value())
    {
        uidText = env("PKEXEC_UID");
    }
    std::uint32_t uid = 0;
    if (!uidText.has_value() || uidText->empty() ||
        std::from_chars(uidText->data(), uidText->data() + uidText->size(), uid).ptr != uidText->data() + uidText->size() || uid == 0)
    {
        return std::nullopt;
    }
    const std::optional<Account> user = account(static_cast<uid_t>(uid));
    if (!user.has_value())
    {
        return std::nullopt;
    }

    DetachedSpawn::RunAs runAs{.uid = static_cast<uid_t>(uid), .gid = user->gid, .environment = {}};
    std::vector<std::string>& environment = runAs.environment;
    environment.push_back("HOME=" + user->home);
    environment.push_back("USER=" + user->name);
    environment.push_back("LOGNAME=" + user->name);
    if (!user->shell.empty())
    {
        environment.push_back("SHELL=" + user->shell);
    }
    for (const std::string_view name : KEPT_VARIABLES)
    {
        if (const std::optional<std::string> value = env(name); value.has_value())
        {
            environment.push_back(std::string(name) + "=" + *value);
        }
    }
    // The user's own runtime directory and session bus, whatever sudo -E passed through: xdg-open asks the
    // desktop over D-Bus, and a relative WAYLAND_DISPLAY is found in XDG_RUNTIME_DIR.
    const std::string runtime = "/run/user/" + std::to_string(uid);
    if (exists(runtime))
    {
        environment.push_back("XDG_RUNTIME_DIR=" + runtime);
        if (exists(runtime + "/bus"))
        {
            environment.push_back("DBUS_SESSION_BUS_ADDRESS=unix:path=" + runtime + "/bus");
        }
    }
    return runAs;
}

/// invokingUserSession() for this process: its effective uid, environment and passwd database.
[[nodiscard]] inline std::optional<DetachedSpawn::RunAs> invokingUserSession()
{
    return invokingUserSession(
        ::geteuid(),
        [](std::string_view name) -> std::optional<std::string>
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe) - read on the UI thread; nothing sets these
            const char* const value = std::getenv(std::string(name).c_str());
            return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
        },
        [](uid_t uid) -> std::optional<Account>
        {
            passwd entry{};
            passwd* found = nullptr;
            std::array<char, 4096> buffer{};
            if (::getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr || entry.pw_dir == nullptr)
            {
                return std::nullopt;
            }
            return Account{.gid = entry.pw_gid,
                           .name = entry.pw_name != nullptr ? entry.pw_name : "",
                           .home = entry.pw_dir,
                           .shell = entry.pw_shell != nullptr ? entry.pw_shell : ""};
        },
        [](const std::string& path) { return ::access(path.c_str(), F_OK) == 0; });
}

} // namespace Platform::SessionUser
