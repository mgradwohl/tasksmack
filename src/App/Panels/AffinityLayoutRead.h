#pragma once

// The CPU layout Process Details' Set affinity dialog groups its boxes by (#1881), read once on a worker
// and kept (#3182). IProcessActions::affinityLayout() calls GetLogicalProcessorInformationEx on Windows
// and reads sysfs on Linux, and the UI thread makes no such call (#2731). The layout doesn't change while
// TaskSmack runs (a CPU hot-add aside, which is rare), so one read serves every opening of the dialog.
// It is started early, from the panel's update path, so it has normally arrived before the first click;
// a dialog opened before then waits for it (ProcessAffinityDialog::openAwaitingLayout()).
//
//     m_LayoutRead.start(actions);                      // the update path; starts the one read
//     if (const auto* layout = m_LayoutRead.layout())   // each frame: non-null once it has arrived
//         dialog.setLayout(*layout);
//
// The Processes table's row menu doesn't need this: its sampler thread reads the layout once, with the
// actions it hands over (ProcessesPanel's PlatformHandoff). Free of ImGui; tested in
// tests/App/test_AffinityLayoutRead.cpp.

#include "App/Panels/BackgroundTask.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <string_view>

namespace App
{

class AffinityLayoutRead
{
  public:
    AffinityLayoutRead() noexcept : m_Task(Platform::AFFINITY_LAYOUT_READ_THREAD_NAME, &failedRead, READ_FAILURE)
    {}

    /// Starts the one read of @p actions' layout on the worker, unless a read was started already or
    /// @p actions is null. @p actions must outlive the read: destroying this waits for it.
    void start(const Platform::IProcessActions* actions)
    {
        if (actions == nullptr || m_Started)
        {
            return;
        }
        m_Started = true;
        // Nullopt once the read has started; the failed (empty) layout at once when no thread could be
        // started for it (logged), and the dialog then goes without one.
        m_Layout = m_Task.start([actions] { return actions->affinityLayout(); });
    }

    /// Whether start() has started the read (or found no thread for it).
    [[nodiscard]] bool started() const noexcept
    {
        return m_Started;
    }

    /// The layout, once the read has finished, taking it in first if it has just done so; null until
    /// then. Never waits, unless @p wait (tests): then it waits for a read still running.
    [[nodiscard]] const Platform::CpuAffinityLayout* layout(bool wait = false)
    {
        if (!m_Layout.has_value())
        {
            m_Layout = m_Task.poll(wait);
        }
        return m_Layout.has_value() ? &m_Layout.value() : nullptr;
    }

  private:
    /// The detail of a read that threw something other than a std::exception.
    static constexpr std::string_view READ_FAILURE = "unknown error";

    /// A read that threw, or that no thread could be started for: logged, and the dialog arranges its
    /// boxes without a layout, as it does for actions that have none.
    [[nodiscard]] static Platform::CpuAffinityLayout failedRead(std::string detail)
    {
        spdlog::warn("Couldn't read the CPU layout for Set affinity: {}", detail);
        return {};
    }

    std::optional<Platform::CpuAffinityLayout> m_Layout;
    bool m_Started = false;
    // Last, so it is destroyed (waiting for a read still running) before the rest.
    BackgroundTask<Platform::CpuAffinityLayout> m_Task;
};

} // namespace App
