#pragma once

// Settings' "Start TaskSmack when I sign in" (#3093, part 2), run: the decisions are Domain::StartAtLogin's,
// the reads and writes Platform::IStartAtLogin's (the HKCU Run key on Windows); this owns the worker they
// run on and the last status it read, and is driven from the UI thread (SettingsLayer). Nothing touches the
// registry on the UI thread (#2731): refresh() reads on the worker, setEnabled() writes or deletes there and
// reads the entry back, and poll() takes the status in.
//
// The entry is the setting: nothing is kept in config.toml. Without a sign-in entry (Linux for now; a
// synthetic or test-hook run) nothing is offered, read or written.
//
// Free of ImGui; tested in tests/App/test_StartAtLoginSetting.cpp with a fake entry.

#include "App/Panels/BackgroundTask.h"
#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"

#include <memory>
#include <optional>

namespace App
{

class StartAtLoginSetting
{
  public:
    /// @param entry  The sign-in entry (Platform::makeStartAtLogin()), or null: nothing is offered.
    explicit StartAtLoginSetting(std::shared_ptr<Platform::IStartAtLogin> entry = nullptr);

    /// Whether the setting is offered here: there is a sign-in entry to read and write.
    [[nodiscard]] bool offered() const noexcept
    {
        return m_Entry != nullptr;
    }

    /// Whether a read or a change is running, or one waits to start.
    [[nodiscard]] bool busy() const noexcept
    {
        return m_Worker.busy() || m_Pending.has_value();
    }

    /// Whether a change (on, off, Repair) runs or waits: not just a read.
    [[nodiscard]] bool changing() const noexcept
    {
        return (m_Worker.busy() && m_Changing) || m_Pending.has_value();
    }

    /// The last status read; State::Unknown before the first read ends.
    [[nodiscard]] const Domain::StartAtLogin::Status& status() const noexcept
    {
        return m_Status;
    }

    /// Reads the entry again, on the worker (Settings opening). Ignored while busy() or when not offered.
    void refresh();

    /// On: writes the entry to start this copy minimized; Off: deletes it. Either way the entry is read
    /// back after. Also Repair (on, over another copy's entry). One that comes while busy() runs after.
    void setEnabled(bool enabled);

    /// On the UI thread, each frame: takes in a finished read or change, and starts one that waited.
    /// @return Whether status() changed.
    [[nodiscard]] bool poll();

    /// Tests: waits for the work running, and any that waited, taking each in.
    void waitForTesting();

  private:
    void start(std::optional<bool> enabled);

    std::shared_ptr<Platform::IStartAtLogin> m_Entry;
    Domain::StartAtLogin::Status m_Status;
    std::optional<bool> m_Pending; ///< A change asked for while busy, started once the worker is free
    bool m_Changing = false;       ///< The work running is a change, not a read
    // Last: destroyed first, waiting for a read or write still running, whose job owns the entry it uses.
    BackgroundTask<Domain::StartAtLogin::Status> m_Worker;
};

} // namespace App
