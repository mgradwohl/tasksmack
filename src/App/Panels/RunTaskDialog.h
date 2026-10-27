#pragma once

// Run new task (#3065): a small dialog for a command line to start, as Task Manager's File > Run new
// task, which is how a hung desktop shell gets started again. A command line (typed, picked from the
// recent ones, or a file chosen with Browse...), and "Run as administrator" where the platform can.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Platform/IProcessActions.h"

#include <imgui.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

/// @p commandLine as the toolbar names it: whole up to 60 bytes, else its first 57 and "...", never cut
/// inside a UTF-8 character.
[[nodiscard]] std::string runTaskShortName(std::string_view commandLine);

/// The toolbar's line for a Run new task: "Started htop", "Started htop as administrator", or "Could not
/// start htop: ..." (#3065).
[[nodiscard]] Detail::ActionResultMessage
runTaskResultMessage(std::string_view shown, bool asAdministrator, const Platform::ProcessActionResult& result);

class RunTaskDialog
{
  public:
    /// Shows a file dialog and hands the chosen path (empty when cancelled) to its argument, maybe later
    /// and on another thread: Core::showOpenFileDialog() in the app, a stand-in in tests.
    using Browse = std::function<void(std::function<void(std::string)>)>;

    RunTaskDialog();
    explicit RunTaskDialog(Browse browse);

    /// Opens the dialog on the next render(), offering @p recent (newest first) and, when
    /// @p canRunAsAdministrator, the administrator tick. The command line is kept from the last time.
    void open(std::vector<std::string> recent, bool canRunAsAdministrator);

    [[nodiscard]] bool isOpen() const noexcept
    {
        return m_ShowRequested || m_Open;
    }

    /// Draws the dialog while it is open. Returns the program to run on the frame Run (or Enter in the
    /// command line) is pressed with a command typed, and closes.
    [[nodiscard]] std::optional<Platform::RunProgramRequest> render();

  private:
    struct BrowseSlot
    {
        std::mutex mutex;
        std::optional<std::string> path; ///< What the file dialog chose, until the next render() takes it
        bool pending = false;            ///< The file dialog is open
    };

    void takeBrowseResult();
    /// The command line, with the recent ones and Browse... beside it; true when Enter was pressed in it.
    [[nodiscard]] bool renderCommandRow();

    Browse m_Browse;
    std::shared_ptr<BrowseSlot> m_BrowseSlot = std::make_shared<BrowseSlot>();
    std::string m_Command;
    std::vector<std::string> m_Recent;
    bool m_AsAdministrator = false;
    bool m_CanRunAsAdministrator = false;
    bool m_ShowRequested = false;
    bool m_Open = false;
    bool m_FocusCommand = false;
    ImVec2 m_LastWorkSize; // For UI::Widgets::centerNextDialog()
};

} // namespace App
