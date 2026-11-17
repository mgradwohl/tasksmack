/// @file test_RootEnvironment.cpp
/// @brief Platform::RootEnvironment (#4158, #4159, #4179): a root session whose environment may be a user's keeps only
/// an allow-list, with root's PATH and HOME. Tested on a fake environment; the process's own is left alone.

#include "Platform/Linux/RootEnvironment.h"

#include <gtest/gtest.h>

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

TEST(RootEnvironmentTest, EverythingElseGoesAndPathAndHomeAreRoots)
{
    const std::array<const char*, 8> environment{
        "DISPLAY=:0",
        "GTK_MODULES=/home/me/evil",
        "PATH=/home/me/bin:/usr/bin",
        "HOME=/home/me",
        "PYTHONPATH=/home/me/py",
        "LC_ALL=C.UTF-8",
        "SUDO_UID=1000",
        nullptr,
    };
    std::vector<std::string> unset;
    std::map<std::string, std::string> set;
    const std::vector<std::string> removed = applyTo(environment.data(),
                                                     std::string("/root"),
                                                     Operations{
                                                         .unset =
                                                             [&unset](const std::string& name)
                                                         {
                                                             unset.push_back(name);
                                                             return true;
                                                         },
                                                         .set =
                                                             [&set](const std::string& name, const std::string& value)
                                                         {
                                                             set[name] = value;
                                                             return true;
                                                         },
                                                     });
    EXPECT_EQ(removed, (std::vector<std::string>{"GTK_MODULES", "PYTHONPATH"}));
    EXPECT_EQ(set.at("PATH"), std::string(ROOT_PATH)) << "#4159: never the user's PATH";
    EXPECT_EQ(set.at("HOME"), "/root") << "root's passwd home, never the user's";
}

TEST(RootEnvironmentTest, AnUnknownRootHomeUnsetsHomeAndAFailedUnsetIsNotReported)
{
    const std::array<const char*, 3> environment{"GTK_MODULES=/x", "HOME=/home/me", nullptr};
    std::vector<std::string> unset;
    const std::vector<std::string> removed = applyTo(environment.data(),
                                                     std::nullopt,
                                                     Operations{
                                                         .unset =
                                                             [&unset](const std::string& name)
                                                         {
                                                             unset.push_back(name);
                                                             return name != "GTK_MODULES";
                                                         },
                                                         .set = [](const std::string&, const std::string&) { return true; },
                                                     });
    EXPECT_TRUE(removed.empty()) << "the unset that failed isn't reported removed";
    EXPECT_EQ(unset, (std::vector<std::string>{"GTK_MODULES", "HOME"}));
}

TEST(RootEnvironmentTest, NotAppliedNothingChanges)
{
    applyToThisProcess(false);
    EXPECT_TRUE(removedFromThisProcess().empty());
}

} // namespace
} // namespace Platform::RootEnvironment
