#pragma once

// A test's own config directory (#2932): each test of a suite whose fixture derives from
// ScratchConfigDirTest starts from a new, empty TASKSMACK_CONFIG_DIR and UserConfig's defaults, so it
// never loads what an earlier test saved. Before this, each --gtest_repeat iteration of the UI scenario
// suite opened on the Processes table layout the last one saved on close (PID 40 px wider each time),
// until PID's right border was out of view and every later iteration failed.
//
// AppTestsMain.cpp points TASKSMACK_CONFIG_DIR at one temporary directory for the whole process.
// Core::ConfigDirOverride::active() reads it once, and UserConfig, a singleton, keeps the config.toml
// path it built from it, so setting the variable again would change nothing. SetUp() instead points
// both at the new directory: active() through setActiveForTesting() (what each Core::Application's
// PathService reads, for the user themes folder), and UserConfig through resetConfigPathForTesting()
// (config.toml, with every setting back to its default). The environment variable isn't touched.
//
// TearDown() points both back at what they were and puts the settings back, then removes the
// directory. resetConfigPathForTesting() first waits for a background save still queued or being
// written (UserConfig::saveInBackground(), #2916), so nothing writes into the directory once it is
// gone. ShellLayer's onDetach() has already saved synchronously by then: the test's Core::Application
// is gone before TearDown() runs.

#include "App/UserConfig.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Utf8Path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace TestSupport
{

/// A fixture that gives each test its own config directory (see the top of this file).
class ScratchConfigDirTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        App::UserConfig& config = App::UserConfig::get();
        m_PreviousOverride = Core::ConfigDirOverride::active();
        m_PreviousConfigPath = config.configPath();
        m_PreviousSettings = config.settings();

        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec) << "no temporary directory: " << ec.message();
        std::random_device random;
        constexpr int MAX_ATTEMPTS = 100;
        for (int attempt = 0; attempt < MAX_ATTEMPTS && m_Dir.empty(); ++attempt)
        {
            std::filesystem::path candidate = base / ("tasksmack_test_config_" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_Dir = std::move(candidate);
            }
        }
        ASSERT_FALSE(m_Dir.empty()) << "couldn't make a scratch config directory under " << Core::pathToUtf8(base);

        Core::ConfigDirOverride::setActiveForTesting(m_Dir);
        config.resetConfigPathForTesting(m_Dir / "config.toml");
        m_Redirected = true;
    }

    void TearDown() override
    {
        if (m_Redirected)
        {
            App::UserConfig& config = App::UserConfig::get();
            config.resetConfigPathForTesting(m_PreviousConfigPath); // waits for a background save first
            config.settings() = m_PreviousSettings;
            Core::ConfigDirOverride::setActiveForTesting(m_PreviousOverride);
            m_Redirected = false;
        }
        if (!m_Dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
            m_Dir.clear();
        }
    }

    /// This test's config directory: empty when the test starts.
    [[nodiscard]] const std::filesystem::path& scratchConfigDir() const noexcept
    {
        return m_Dir;
    }

  private:
    std::filesystem::path m_Dir;
    std::optional<std::filesystem::path> m_PreviousOverride;
    std::filesystem::path m_PreviousConfigPath;
    App::UserSettings m_PreviousSettings;
    bool m_Redirected = false;
};

} // namespace TestSupport
