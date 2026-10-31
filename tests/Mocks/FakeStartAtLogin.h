#pragma once

// A sign-in entry in memory (#3093, part 2), for the tests of App::StartAtLoginSetting and the Settings
// layer: never the registry. Called on the setting's worker, so every member is read under the lock; a test
// sets it up, or reads what was written, between the worker's jobs (or under the lock, with with()).

#include "Platform/IStartAtLogin.h"

#include <chrono>
#include <expected>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace TestSupport
{

class FakeStartAtLogin final : public Platform::IStartAtLogin
{
  public:
    struct Data
    {
        std::optional<std::string> command;                                     ///< The entry's text; nullopt: none
        std::string executable = R"(C:\Program Files\TaskSmack\TaskSmack.exe)"; ///< This copy's path
        std::string readError;                                                  ///< Non-empty: read() fails with it
        std::string writeError;                                                 ///< Non-empty: write() fails with it
        std::string removeError;                                                ///< Non-empty: remove() fails with it
        std::optional<std::shared_future<void>> hold;                           ///< Set: every call waits for it first
        int reads = 0;
        int writes = 0;
        int removes = 0;
        bool installed = true;
    };

    /// Runs @p change on the data under the lock, and returns what it returns.
    template<typename Change> auto with(Change change)
    {
        const std::scoped_lock lock(m_Mutex);
        return change(m_Data);
    }

    [[nodiscard]] std::expected<Platform::StartAtLoginFacts, std::string> read() override
    {
        waitForHold();
        const std::scoped_lock lock(m_Mutex);
        ++m_Data.reads;
        if (!m_Data.readError.empty())
        {
            return std::unexpected(m_Data.readError);
        }
        return Platform::StartAtLoginFacts{
            .command = m_Data.command, .executable = m_Data.executable, .installed = m_Data.installed, .caseInsensitivePaths = true};
    }

    [[nodiscard]] std::expected<void, std::string> write(std::string_view command) override
    {
        waitForHold();
        const std::scoped_lock lock(m_Mutex);
        ++m_Data.writes;
        if (!m_Data.writeError.empty())
        {
            return std::unexpected(m_Data.writeError);
        }
        m_Data.command = std::string(command);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> remove() override
    {
        waitForHold();
        const std::scoped_lock lock(m_Mutex);
        ++m_Data.removes;
        if (!m_Data.removeError.empty())
        {
            return std::unexpected(m_Data.removeError);
        }
        m_Data.command.reset();
        return {};
    }

  private:
    void waitForHold()
    {
        std::optional<std::shared_future<void>> hold;
        {
            const std::scoped_lock lock(m_Mutex);
            hold = m_Data.hold;
        }
        if (hold.has_value())
        {
            // Bounded, so a test that fails before releasing it still tears down.
            static_cast<void>(hold->wait_for(std::chrono::seconds(30)));
        }
    }

    std::mutex m_Mutex;
    Data m_Data;
};

} // namespace TestSupport
