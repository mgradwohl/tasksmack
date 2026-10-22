#pragma once

// Fails a TaskSmackAppTests test that logged a warning or worse it didn't expect (#2718). These tests
// run the real Core::Application, its layers and panels on a display; #1784 checked by hand that window
// interaction logs nothing at warning level or above, and this makes every on-display run check it.
//
// One spdlog sink is added to the default logger once, before any test runs (adding and removing
// sinks while a worker might be logging isn't safe); it records warn-and-above lines only while a test
// runs. When the test ends, any recorded line that no ALLOWED_WARNINGS entry matches fails it, listing
// the lines. A test that swaps in its own default logger (to check its own messages) bypasses this.

#include <gtest/gtest.h>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TestSupport
{

/// A warning the test environment produces whatever the product does, matched as a substring.
struct AllowedWarning
{
    std::string_view contains; ///< Text the logged line contains
    std::string_view why;      ///< Why it is expected in tests (kept with the entry, not elsewhere)
};

/// Every expected warning, in one place. Add an entry only for the environment, never to hide a
/// product warning a test provokes by accident; a test that checks a warning is logged should capture
/// it with its own logger instead.
inline constexpr std::array ALLOWED_WARNINGS{
    AllowedWarning{.contains = "TASKSMACK_SYNTHETIC is set",
                   .why = "tests that use the synthetic machine announce it, as the app does for any user who sets it"},
    AllowedWarning{.contains = "No GPU probe provided",
                   .why = "the composed shell in tests has no GPU probe on machines and runners without a GPU"},
    AllowedWarning{.contains = "icon from resource",
                   .why = "on Windows the icon resource is linked into TaskSmack.exe only, not the test binary (#2039)"},
};

/// The lines of @p logged that no entry of @p allowed matches.
[[nodiscard]] inline std::vector<std::string> unexpectedWarnings(const std::vector<std::string>& logged,
                                                                 std::span<const AllowedWarning> allowed)
{
    std::vector<std::string> unexpected;
    for (const std::string& line : logged)
    {
        bool expected = false;
        for (const AllowedWarning& entry : allowed)
        {
            if (std::string_view(line).contains(entry.contains))
            {
                expected = true;
                break;
            }
        }
        if (!expected)
        {
            unexpected.push_back(line);
        }
    }
    return unexpected;
}

namespace Detail
{

/// What the sink records and the listener checks. The sink may run on any thread, even after a test ends.
struct LogGuardState
{
    std::atomic<bool> recording{false};
    std::mutex mutex;
    std::vector<std::string> lines;    ///< warn-and-above lines logged during this test
    std::vector<std::string> expected; ///< expectLogWarning() texts for this test
};

/// The one state install() creates; null until then, so expectLogWarning() works only under the guard.
inline std::shared_ptr<LogGuardState>& logGuardState() noexcept
{
    static std::shared_ptr<LogGuardState> state; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - the guard's single instance
    return state;
}

} // namespace Detail

/// Declares, from inside a test, a warning the test provokes on purpose, matched as a substring. It
/// counts as expected for this test only, and the test fails if no logged line contains it, so the
/// declaration also checks that the warning is logged.
inline void expectLogWarning(std::string_view contains)
{
    if (const std::shared_ptr<Detail::LogGuardState>& state = Detail::logGuardState(); state != nullptr)
    {
        const std::scoped_lock lock(state->mutex);
        state->expected.emplace_back(contains);
    }
}

/// The listener: records while a test runs, checks when it ends. Install() once, before RUN_ALL_TESTS().
class LogWarningGuard final : public ::testing::EmptyTestEventListener
{
  public:
    /// Adds the recording sink to spdlog's default logger and this listener to googletest, which owns it.
    static void install()
    {
        auto* guard = new LogWarningGuard(); // NOLINT(cppcoreguidelines-owning-memory) - googletest's listener list takes ownership
        spdlog::default_logger()->sinks().push_back(guard->m_Sink);
        Detail::logGuardState() = guard->m_State;
        ::testing::UnitTest::GetInstance()->listeners().Append(guard);
    }

    void OnTestStart(const ::testing::TestInfo& /*info*/) override
    {
        {
            const std::scoped_lock lock(m_State->mutex);
            m_State->lines.clear();
            m_State->expected.clear();
        }
        m_State->recording = true;
    }

    void OnTestEnd(const ::testing::TestInfo& /*info*/) override
    {
        m_State->recording = false;
        std::vector<std::string> logged;
        std::vector<std::string> expected;
        {
            const std::scoped_lock lock(m_State->mutex);
            logged = std::exchange(m_State->lines, {});
            expected = std::exchange(m_State->expected, {});
        }
        std::vector<AllowedWarning> allowed(ALLOWED_WARNINGS.begin(), ALLOWED_WARNINGS.end());
        for (const std::string& text : expected)
        {
            allowed.push_back({.contains = text, .why = "declared by the test with expectLogWarning()"});
            if (!std::ranges::any_of(logged, [&text](const std::string& line) { return line.contains(text); }))
            {
                ADD_FAILURE() << "the test declared the warning \"" << text << "\" with expectLogWarning(), but it was never logged";
            }
        }
        const std::vector<std::string> unexpected = unexpectedWarnings(logged, allowed);
        if (!unexpected.empty())
        {
            std::string list;
            for (const std::string& line : unexpected)
            {
                list += "\n  " + line;
            }
            ADD_FAILURE() << unexpected.size() << " unexpected warning(s) or error(s) logged during the test (#2718); fix the cause, "
                          << "declare one the test provokes on purpose with TestSupport::expectLogWarning(), or add an "
                          << "environment-only entry to TestSupport::ALLOWED_WARNINGS:" << list;
        }
    }

  private:
    LogWarningGuard()
        : m_State(std::make_shared<Detail::LogGuardState>()),
          m_Sink(std::make_shared<spdlog::sinks::callback_sink_mt>(
              [state = m_State](const spdlog::details::log_msg& msg)
              {
                  if (msg.level < spdlog::level::warn || !state->recording)
                  {
                      return;
                  }
                  const spdlog::string_view_t level = spdlog::level::to_string_view(msg.level);
                  std::string line = std::string(level.data(), level.size()) + ": " + std::string(msg.payload.data(), msg.payload.size());
                  const std::scoped_lock lock(state->mutex);
                  state->lines.push_back(std::move(line));
              }))
    {}

    std::shared_ptr<Detail::LogGuardState> m_State;
    std::shared_ptr<spdlog::sinks::callback_sink_mt> m_Sink;
};

} // namespace TestSupport
