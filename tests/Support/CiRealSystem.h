#pragma once

/// @file CiRealSystem.h
/// @brief The guard for the real-system tests (#3489; tracker #3492): tests that change the machine they run on
/// (a process at Realtime, a helper served from Program Files, a throwaway account, an Explorer window), which
/// only a disposable CI runner may run.
///
/// Every such test is in a suite named CiRealSystem*, derived from CiRealSystemTest, so it skips unless both:
///   - TASKSMACK_CI_REAL_SYSTEM=1, which only the CI step that runs the group sets
///     (.github/workflows/reusable-build-test.yml, "Real-system tests (Windows)");
///   - the runner looks disposable: GITHUB_ACTIONS=true and RUNNER_ENVIRONMENT=github-hosted, which a
///     self-hosted runner or a developer's machine doesn't have.
/// The daytime test filter excludes CiRealSystem* as well (AGENTS.md), and that step fails when the whole group
/// skipped, so a guard that stops seeing CI can't pass as a run of skips. Never set the variable by hand.

#include <gtest/gtest.h>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#include <array>
#else
#include <cstdlib>
#endif

#include <optional>
#include <string>
#include <string_view>

namespace TestSupport
{

/// The variable the CI step sets to ask for the real-system tests.
inline constexpr std::string_view CI_REAL_SYSTEM_VARIABLE = "TASKSMACK_CI_REAL_SYSTEM";

/// @p name's value, or nullopt when it isn't set.
[[nodiscard]] inline std::optional<std::string> environmentValue(const std::string& name)
{
#ifdef _WIN32
    std::array<char, 256> value{};
    const DWORD length = GetEnvironmentVariableA(name.c_str(), value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
    {
        return std::nullopt; // not set, or longer than any value the guard accepts
    }
    return std::string(value.data(), length);
#else
    const char* value = std::getenv(name.c_str()); // NOLINT(concurrency-mt-unsafe) - read before the test starts threads
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

/// Why the real-system tests must not run here, or nullopt on a disposable CI runner that asked for them.
[[nodiscard]] inline std::optional<std::string> ciRealSystemSkipReason()
{
    if (environmentValue(std::string(CI_REAL_SYSTEM_VARIABLE)) != "1")
    {
        return std::string("a CI real-system test: it changes this machine, so it runs only on a disposable "
                           "GitHub-hosted runner with TASKSMACK_CI_REAL_SYSTEM=1 (#3489)");
    }
    if (environmentValue("GITHUB_ACTIONS") != "true" || environmentValue("RUNNER_ENVIRONMENT") != "github-hosted")
    {
        return std::string("a CI real-system test: TASKSMACK_CI_REAL_SYSTEM is set, but this isn't a disposable "
                           "GitHub-hosted runner (GITHUB_ACTIONS=true, RUNNER_ENVIRONMENT=github-hosted), so it "
                           "doesn't run (#3489)");
    }
    return std::nullopt;
}

/// GTEST_SKIP()s the current test unless ciRealSystemSkipReason() lets it run. Called from a fixture's SetUp(),
/// as CiRealSystemTest does, the test body then never runs: gtest skips it once SetUp() has recorded a skip.
inline void requireCiRealSystem()
{
    if (const std::optional<std::string> why = ciRealSystemSkipReason())
    {
        GTEST_SKIP() << *why;
    }
}

/// The base of every CiRealSystem* suite: its SetUp() is the guard.
class CiRealSystemTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        requireCiRealSystem();
    }
};

} // namespace TestSupport
