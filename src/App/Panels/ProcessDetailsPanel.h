#pragma once

#include "App/Panel.h"
#include "Domain/History.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessActions.h"

#include <cstdint>
#include <memory>
#include <string>

namespace App
{

/// Panel displaying detailed information for a selected process.
/// Includes resource usage, I/O stats, history graphs, and process actions.
class ProcessDetailsPanel : public Panel
{
  public:
    ProcessDetailsPanel();
    ~ProcessDetailsPanel() override = default;

    ProcessDetailsPanel(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel& operator=(const ProcessDetailsPanel&) = delete;
    ProcessDetailsPanel(ProcessDetailsPanel&&) = default;
    ProcessDetailsPanel& operator=(ProcessDetailsPanel&&) = default;

    /// Update with current process data.
    /// Call each frame with the snapshot for the selected process (or nullptr if none).
    void updateWithSnapshot(const Domain::ProcessSnapshot* snapshot, float deltaTime);

    /// Render the panel.
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

    /// Set the process to display.
    void setSelectedPid(int32_t pid);

    /// Get currently displayed PID.
    [[nodiscard]] int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

  private:
    void renderBasicInfo(const Domain::ProcessSnapshot& proc);
    void renderResourceUsage(const Domain::ProcessSnapshot& proc);
    void renderIoStats(const Domain::ProcessSnapshot& proc);
    void renderHistoryGraphs();
    void renderActions();

    int32_t m_SelectedPid = -1;
    float m_HistoryTimer = 0.0F;

    // History buffers (120 samples at 1 Hz = 2 minutes of data)
    static constexpr size_t HISTORY_SIZE = 120;
    static constexpr float HISTORY_SAMPLE_INTERVAL = 1.0F;

    Domain::History<float, HISTORY_SIZE> m_CpuHistory;
    Domain::History<float, HISTORY_SIZE> m_MemoryHistory; // In MB

    // Cached snapshot for rendering
    Domain::ProcessSnapshot m_CachedSnapshot;
    bool m_HasSnapshot = false;

    // Process actions
    std::unique_ptr<Platform::IProcessActions> m_ProcessActions;
    Platform::ProcessActionCapabilities m_ActionCapabilities;

    // Confirmation dialog state
    bool m_ShowConfirmDialog = false;
    std::string m_ConfirmAction;
    std::string m_LastActionResult;
    float m_ActionResultTimer = 0.0F;
};

} // namespace App
