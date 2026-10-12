/// @file AppTestsMain.cpp
/// @brief main() for TaskSmackAppTests (#1395): the tests run a real Core::Application, whose
/// PathService and UserConfig read TASKSMACK_CONFIG_DIR once per process, on first use. Pointing it at
/// a fresh temporary directory before any test runs means nothing these tests save (ShellLayer's
/// onDetach() writes config.toml) can reach the user's own config. The directory is removed afterwards.

#include "Core/ConfigDirOverride.h"
#include "Core/Utf8Path.h"

#include <SDL3/SDL_stdinc.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace
{

/// A new, empty directory under the system's temporary directory, or an empty path on failure.
std::filesystem::path makeTempDir()
{
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        return {};
    }
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        std::filesystem::path candidate = base / ("tasksmack_app_tests_" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate, ec) && !ec)
        {
            return candidate;
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        ::testing::InitGoogleTest(&argc, argv);
        const std::filesystem::path configDir = makeTempDir();
        if (configDir.empty())
        {
            std::fputs("TaskSmackAppTests: couldn't make a temporary config directory\n", stderr);
            return 1;
        }
        // SDL's setter: ConfigDirOverride reads it back with SDL_getenv(), which this updates too (UTF-8).
        const std::string name(Core::ConfigDirOverride::ENV_VAR);
        if (SDL_setenv_unsafe(name.c_str(), Core::pathToUtf8(configDir).c_str(), 1) != 0)
        {
            std::fputs("TaskSmackAppTests: couldn't set TASKSMACK_CONFIG_DIR\n", stderr);
            return 1;
        }
        const int result = RUN_ALL_TESTS();
        std::error_code ec;
        std::filesystem::remove_all(configDir, ec);
        return result;
    }
    catch (const std::exception& e)
    {
        std::fputs("TaskSmackAppTests: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}
