#pragma once

// The layer behind TASKSMACK_DEMO (#2827), pushed only when the hook is set (so an ordinary launch pays
// nothing), after the dialog layers and before ScreenshotLayer. It replays the script's steps on the
// app clock, each through the request path the UI or a startup hook would take: the shell's tab
// request (as TASKSMACK_TAB), a selection waiting for its process (as TASKSMACK_SELECT_PID/_NAME,
// then as a click), the ShowProcessDetailsEvent the row menu's Details raises, the machine tab's
// sub-tab request (as TASKSMACK_MACHINE_TAB, #2869), the Open*Event the title bar's buttons raise, the
// theme change Settings makes, and a Process Details section drawn open through ImGui's own open
// state. Nothing is injected as input.
//
// The clock starts at the settle point: the first frame on which the shell reports no startup
// selection pending, read in onPostRender() as ScreenshotLayer reads it, so the two layers agree on
// the frame (a sequence with after=0 captures its first frame SETTLE_FRAMES frames later). Steps are
// issued in onUpdate(), which runs before every layer's onRender(), so the shell's tab bar and panels
// take a step's request on the frame its time arrives.
//
// The demo's requests never reset the screenshot's settle: ScreenshotLayer latches its settled state
// once and never re-reads startupSelectionPending(); and the demo's tab and selection requests live
// in slots of their own (ShellLayer's demo tab, ProcessesPanel's requested selection) that
// startupSelectionPending() does not count -- so a screenshot whose `after` is still running when a
// step asks for a tab, or for a process that never appears, is not held up by it.

#include "App/DemoOverride.h"
#include "Core/Layer.h"

#include <cstddef>
#include <vector>

namespace App
{

class ShellLayer;

class DemoLayer : public Core::Layer
{
  public:
    /// @p shell: the shell whose startup selections start the clock and whose requests the steps make.
    /// Non-owning: the application's layer stack holds both for the application's lifetime.
    DemoLayer(std::vector<DemoOverride::Step> steps, ShellLayer& shell);
    ~DemoLayer() override = default;

    DemoLayer(const DemoLayer&) = delete;
    DemoLayer& operator=(const DemoLayer&) = delete;
    DemoLayer(DemoLayer&&) = delete;
    DemoLayer& operator=(DemoLayer&&) = delete;

    void onUpdate(float deltaTime) override;
    void onPostRender() override;

    /// Whether the clock has started: the startup selections have settled.
    [[nodiscard]] bool started() const noexcept
    {
        return m_Started;
    }

    /// Steps issued so far, in script order.
    [[nodiscard]] std::size_t stepsIssued() const noexcept
    {
        return m_Next;
    }

  private:
    /// Makes one step's request; a tab or theme the step names but the app doesn't have is logged and skipped.
    void issue(const DemoOverride::Step& step) const;

    std::vector<DemoOverride::Step> m_Steps;
    ShellLayer* m_Shell = nullptr; ///< Never null
    bool m_Started = false;
    double m_StartTime = 0.0; ///< App clock at the settle point; the steps' times count from it
    std::size_t m_Next = 0;   ///< The next step to issue
};

} // namespace App
