/// @file test_LinuxSdlEnvironment.cpp
/// @brief Platform::SdlEnvironment (#4111): a session run as root for a user removes every SDL variable from its
/// environment before its first SDL call, so none of the user's names a library SDL loads as root. Tested on a fake
/// environment; the process's own is left alone.

#include "Platform/Linux/SdlEnvironment.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace Platform::SdlEnvironment
{
namespace
{

TEST(LinuxSdlEnvironmentTest, SdlVariablesAreSdlAndSdlDigitsPrefixedNamesInExactCase)
{
    for (const char* name : {"SDL_OPENGL_LIBRARY", "SDL_EGL_LIBRARY", "SDL3_DYNAMIC_API", "SDL2_FOO", "SDL_VIDEO_DRIVER", "SDL_"})
    {
        EXPECT_TRUE(isSdlVariable(name)) << name;
    }
    for (const char* name : {"SDL", "SDLX_FOO", "sdl_opengl_library", "MY_SDL_LIB", "SDL3", "HOME", ""})
    {
        EXPECT_FALSE(isSdlVariable(name)) << name;
    }
}

TEST(LinuxSdlEnvironmentTest, TheGraphicsLoaderVariablesAreRemovedTooInExactCase)
{
    // #4151: the libraries SDL's GL/EGL window loads read these to pick a library to load, as root under sudo -E.
    for (const char* name : {"LIBGL_DRIVERS_PATH",
                             "__EGL_VENDOR_LIBRARY_FILENAMES",
                             "__GLX_VENDOR_LIBRARY_NAME",
                             "__GLX_FORCE_VENDOR_LIBRARY_0",
                             "LIBDECOR_PLUGIN_DIR",
                             "VK_ICD_FILENAMES",
                             "GBM_BACKENDS_PATH"})
    {
        EXPECT_TRUE(isLoaderVariable(name)) << name;
    }
    for (const char* name :
         {"libgl_drivers_path", "LIBGL_DRIVERS", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "LIBGL_ALWAYS_SOFTWARE", ""})
    {
        EXPECT_FALSE(isLoaderVariable(name)) << name;
    }
    const std::array<const char*, 5> environment{
        "DISPLAY=:0",
        "LIBGL_DRIVERS_PATH=/home/me/evil",
        "SDL_VIDEO_DRIVER=x11",
        "__EGL_VENDOR_LIBRARY_FILENAMES=/home/me/vendor.json",
        nullptr,
    };
    const std::vector<std::string> expected{"LIBGL_DRIVERS_PATH", "SDL_VIDEO_DRIVER", "__EGL_VENDOR_LIBRARY_FILENAMES"};
    EXPECT_EQ(sdlVariableNames(environment.data()), expected) << "the display variables stay: the window needs them";
}

TEST(LinuxSdlEnvironmentTest, OnlyTheSdlVariablesAreRemovedAndNamedNeverTheirValues)
{
    const std::array<const char*, 6> environment{
        "HOME=/home/me",
        "SDL_OPENGL_LIBRARY=/home/me/libfake.so",
        "SDL3_DYNAMIC_API=/home/me/evil.so",
        "PATH=/usr/bin",
        "SDL_NOVALUE",
        nullptr,
    };
    std::vector<std::string> unset;
    const std::vector<std::string> removed = removeSdlVariables(environment.data(),
                                                                [&unset](const std::string& name)
                                                                {
                                                                    unset.push_back(name);
                                                                    return true;
                                                                });
    const std::vector<std::string> expected{"SDL_OPENGL_LIBRARY", "SDL3_DYNAMIC_API", "SDL_NOVALUE"};
    EXPECT_EQ(unset, expected);
    EXPECT_EQ(removed, expected);
    for (const std::string& name : removed)
    {
        EXPECT_FALSE(name.contains('=')) << "a name, never NAME=value: " << name;
    }
}

TEST(LinuxSdlEnvironmentTest, AFailedUnsetIsNotReportedRemovedAndNoEnvironmentIsNothing)
{
    const std::array<const char*, 3> environment{"SDL_EGL_LIBRARY=/x", "SDL_X11_XCB_LIBRARY=/y", nullptr};
    const std::vector<std::string> removed =
        removeSdlVariables(environment.data(), [](const std::string& name) { return name != "SDL_EGL_LIBRARY"; });
    EXPECT_EQ(removed, std::vector<std::string>{"SDL_X11_XCB_LIBRARY"});
    EXPECT_TRUE(sdlVariableNames(nullptr).empty());
}

TEST(LinuxSdlEnvironmentTest, NotAsRootForAUserNothingIsRemoved)
{
    removeForThisProcess(false);
    EXPECT_TRUE(removedFromThisProcess().empty());
}

} // namespace
} // namespace Platform::SdlEnvironment
