/// @file test_RootEnvironment.cpp
/// @brief Platform::RootEnvironment (#4158, #4159, #4179): a root session whose environment may be a user's keeps only
/// an allow-list, with root's PATH and HOME. Tested on a fake environment; the process's own is left alone.

#include "Platform/Linux/RootEnvironment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Platform::RootEnvironment
{
namespace
{

TEST(RootEnvironmentTest, TheDisplayLocaleTerminalAndWhoStartedItAreKept)
{
    for (const char* name : {"DISPLAY",
                             "WAYLAND_DISPLAY",
                             "XAUTHORITY",
                             "XDG_RUNTIME_DIR",
                             "LANG",
                             "LC_ALL",
                             "LC_TIME",
                             "TERM",
                             "SUDO_UID",
                             "PKEXEC_UID",
                             "DOAS_USER",
                             "TASKSMACK_LOG_LEVEL",
                             "TASKSMACK_SCREENSHOT"})
    {
        EXPECT_TRUE(isKept(name)) << name;
    }
    // The ones a list of bad names kept missing (#4158, #4159, #4179), and the user's folders.
    for (const char* name : {"GTK_MODULES",
                             "GIO_EXTRA_MODULES",
                             "GDK_PIXBUF_MODULE_FILE",
                             "PYTHONPATH",
                             "SDL_OPENGL_LIBRARY",
                             "LIBGL_DRIVERS_PATH",
                             "DBUS_SESSION_BUS_ADDRESS",
                             "XDG_CONFIG_HOME",
                             "XDG_DATA_HOME",
                             "TMPDIR",
                             "LD_PRELOAD",
                             "display",
                             "lc_all",
                             ""})
    {
        EXPECT_FALSE(isKept(name)) << name;
    }
}

/// A fake environment's operations, recording every unset and set.
struct Recorder
{
    std::vector<std::string> unset;
    std::map<std::string, std::string> set;
    std::string failSet; ///< a name whose set fails
    [[nodiscard]] Operations operations()
    {
        return Operations{
            .unset =
                [this](const std::string& name)
            {
                unset.push_back(name);
                return true;
            },
            .set =
                [this](const std::string& name, const std::string& value)
            {
                if (name == failSet)
                {
                    return false;
                }
                set[name] = value;
                return true;
            },
        };
    }
};

TEST(RootEnvironmentTest, EverythingElseGoesAndRootsValuesReplaceTheUsers)
{
    const std::array<const char*, 12> environment{
        "DISPLAY=:0",
        "GTK_MODULES=/home/me/evil",
        "PATH=/home/me/bin:/usr/bin",
        "HOME=/home/me",
        "PYTHONPATH=/home/me/py",
        "LC_ALL=C.UTF-8",
        "SUDO_UID=1000",
        "XDG_DATA_DIRS=/home/me/x:/usr/share",
        "XDG_RUNTIME_DIR=/run/user/1000",
        "WAYLAND_DISPLAY=wayland-0",
        "NOVALUE",
        nullptr,
    };
    Recorder recorder;
    const std::vector<std::string> removed =
        applyTo(environment.data(),
                RootValues{.home = "/root", .runtimeDir = "/run/user/0", .xauthority = "/run/user/0/tasksmack-xauthority"},
                recorder.operations());
    EXPECT_EQ(removed, (std::vector<std::string>{"GTK_MODULES", "PYTHONPATH", "NOVALUE"}));
    for (const char* kept : {"DISPLAY", "LC_ALL", "SUDO_UID"})
    {
        EXPECT_EQ(std::ranges::find(recorder.unset, kept), recorder.unset.end()) << kept << " is kept, never unset";
    }
    EXPECT_EQ(recorder.set.at("PATH"), std::string(ROOT_PATH)) << "#4159: never the user's PATH";
    EXPECT_EQ(recorder.set.at("HOME"), "/root");
    EXPECT_EQ(recorder.set.at("XDG_DATA_DIRS"), std::string(ROOT_DATA_DIRS)) << "the #4187 review: Vulkan layers, schemas";
    EXPECT_EQ(recorder.set.at("WAYLAND_DISPLAY"), "/run/user/1000/wayland-0") << "the user's socket, by its absolute path";
    EXPECT_EQ(recorder.set.at("XDG_RUNTIME_DIR"), "/run/user/0") << "root's own, where dconf and the like write";
    EXPECT_EQ(recorder.set.at("XAUTHORITY"), "/run/user/0/tasksmack-xauthority")
        << "root's own copy of the user's cookie, never a file the user names (the second #4187 review)";
}

TEST(RootEnvironmentTest, UnknownRootValuesAndAFailedSetUnsetRatherThanKeepTheUsers)
{
    const std::array<const char*, 5> environment{"DISPLAY=:0", "HOME=/home/me", "XDG_RUNTIME_DIR=/run/user/1000", "XAUTHORITY=/x", nullptr};
    Recorder recorder;
    recorder.failSet = "PATH";
    const std::vector<std::string> removed =
        applyTo(environment.data(),
                RootValues{.home = std::nullopt, .runtimeDir = std::nullopt, .xauthority = std::nullopt},
                recorder.operations());
    EXPECT_TRUE(removed.empty());
    for (const char* name : {"XDG_RUNTIME_DIR", "HOME", "PATH", "XAUTHORITY"})
    {
        EXPECT_NE(std::ranges::find(recorder.unset, name), recorder.unset.end()) << name << " unset, not left the user's";
    }
    EXPECT_FALSE(recorder.set.contains("XAUTHORITY")) << "with DISPLAY set, a user-named Xauthority goes when root has no copy";
    // Every replaced name is unset before it's set: no duplicate entry survives.
    EXPECT_NE(std::ranges::find(recorder.unset, "XDG_DATA_DIRS"), recorder.unset.end());
}

TEST(RootEnvironmentTest, OnlyALocaleNameNotAPathIsKept)
{
    // #4256: as sudo's env_check, a locale value with "/" or "%", or starting with ".", goes; glibc could take it as a
    // path to locale data the user chose.
    for (const char* name : {"LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES", "TASKSMACK_LOCALE"})
    {
        EXPECT_TRUE(isLocaleVariable(name)) << name;
    }
    EXPECT_FALSE(isLocaleVariable("LANGS"));
    EXPECT_FALSE(isLocaleVariable("TASKSMACK_DEMO"));
    for (const char* value : {"en_US.UTF-8", "C.UTF-8", "C", "de_DE@euro", "en_GB:en", ""})
    {
        EXPECT_TRUE(isSafeLocale(value)) << value;
    }
    for (const char* value : {"/home/alice/loc", "../../home/alice/loc", ".hidden", "en_US%2F", "C/../x"})
    {
        EXPECT_FALSE(isSafeLocale(value)) << value;
    }
    const std::array<const char*, 5> environment{
        "LANG=en_US.UTF-8", "LC_ALL=/home/alice/loc", "LANGUAGE=../x", "TASKSMACK_LOCALE=.evil", nullptr};
    Recorder recorder;
    EXPECT_EQ(applyTo(environment.data(), RootValues{}, recorder.operations()),
              (std::vector<std::string>{"LC_ALL", "LANGUAGE", "TASKSMACK_LOCALE"}))
        << "a plain LANG stays";
}

TEST(RootEnvironmentTest, OnlyAPlainOrZoneinfoTimeZoneIsKept)
{
    for (const char* tz : {"UTC", "Europe/Paris", ":Europe/Paris", "/usr/share/zoneinfo/UTC", ":/usr/share/zoneinfo/Asia/Tokyo"})
    {
        EXPECT_TRUE(isSafeTimeZone(tz)) << tz;
    }
    for (const char* tz : {"/home/u/f", ":../../../home/u/f", "/usr/share/zoneinfo/../../../home/u/f", "/etc/localtime"})
    {
        EXPECT_FALSE(isSafeTimeZone(tz)) << tz;
    }
    const std::array<const char*, 2> environment{"TZ=/home/u/f", nullptr};
    Recorder recorder;
    EXPECT_EQ(applyTo(environment.data(), RootValues{}, recorder.operations()), (std::vector<std::string>{"TZ"}));
}

} // namespace
} // namespace Platform::RootEnvironment
