#pragma once

// Opens a link, file or folder with the system's handler on a worker, never on the UI thread (#3155):
// About's and Help's links, Settings' "Edit Config File" and "Open Themes Folder", and the dump result's
// "Open folder". ShellExecuteW (Windows) can load shell extensions and wait on COM, or on the network for
// a folder on a share; xdg-open (Linux) is a PATH scan and a fork and exec. Any of them can take long
// enough to freeze a frame, and the UI thread never makes a blocking call (#2731).
//
// One open at a time, on a BackgroundTask (ts-sys-open): start() refuses a second while one runs, so a
// double click opens one window, and the view shows busy() as its pending state (its link or button
// disabled). The job may do what the open needs first, on the same worker: make the folder, or write a
// file that isn't there yet. Its failure is the platform opener's warning in the log, as before; a job
// that throws, or one no thread could be started for, is logged here.
//
//     BackgroundOpen m_Open;
//     ImGui::BeginDisabled(m_Open.busy());  // each frame, after m_Open.poll()
//     if (clicked) static_cast<void>(m_Open.start(BackgroundOpen::openUtf8(url)));
//
// Free of ImGui; tested in tests/App/test_BackgroundOpen.cpp.

#include "App/Panels/BackgroundTask.h"
#include "App/PlatformOpen.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

class BackgroundOpen
{
  public:
    /// An open, and anything it needs first; runs on the worker. True when the handler was started.
    using Job = std::function<bool()>;

    /// @param threadName The worker's name (Platform/ThreadName.h); must outlive this.
    explicit BackgroundOpen(std::string_view threadName = Platform::SYSTEM_OPEN_THREAD_NAME) noexcept
        : m_Task(threadName, &failedOpen, OPEN_FAILURE)
    {}

    /// Opens @p target (a URL or other UTF-8 string) with the system handler (PlatformOpen).
    [[nodiscard]] static Job openUtf8(std::string target)
    {
        return [target = std::move(target)]
        {
            return PlatformOpen::openWithSystemHandler(std::string_view{target});
        };
    }

    /// Opens @p path with the system handler (PlatformOpen), in its native form.
    [[nodiscard]] static Job openPath(std::filesystem::path path)
    {
        return [path = std::move(path)]
        {
            return PlatformOpen::openWithSystemHandler(path);
        };
    }

    /// Starts @p job on the worker, after taking in an open that has finished. It owns (copies, or
    /// shares) everything it uses. Refused while another still runs: the second click of a double click.
    /// @return true when it started; false when one still runs, or no thread could be started (logged).
    [[nodiscard]] bool start(Job job)
    {
        static_cast<void>(poll());
        if (m_Task.busy())
        {
            return false;
        }
        // A failure to start is the Failed result, which failedOpen() has logged.
        return !m_Task.start(std::move(job)).has_value();
    }

    /// Whether an open was started and not taken in by poll() yet: the view's pending state.
    [[nodiscard]] bool busy() const noexcept
    {
        return m_Task.busy();
    }

    /// The finished open's result (true: the handler was started), once; nullopt while none has
    /// finished. With @p wait, waits for one still running first. Call it each frame, so busy() ends.
    std::optional<bool> poll(bool wait = false)
    {
        return m_Task.poll(wait);
    }

    /// Waits for an open still running, leaving its result for poll() (a layer's teardown).
    void wait() noexcept
    {
        m_Task.wait();
    }

  private:
    /// The detail of a job that threw something other than a std::exception.
    static constexpr std::string_view OPEN_FAILURE = "unknown error";

    /// A job that threw, or that no thread could be started for: logged, as the opener logs its own.
    [[nodiscard]] static bool failedOpen(std::string detail)
    {
        spdlog::warn("Couldn't open with the system handler: {}", detail);
        return false;
    }

    // Destroying this waits for an open still running; its job owns all it uses.
    BackgroundTask<bool> m_Task;
};

} // namespace App
