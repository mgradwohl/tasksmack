#pragma once

#include "App/Panel.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace App
{

/// Panel for displaying and managing the process list.
/// Uses background sampling for non-blocking updates.
class ProcessesPanel : public Panel
{
  public:
    ProcessesPanel();
    ~ProcessesPanel() override;

    ProcessesPanel(const ProcessesPanel&) = delete;
    ProcessesPanel& operator=(const ProcessesPanel&) = delete;
    ProcessesPanel(ProcessesPanel&&) = delete;
    ProcessesPanel& operator=(ProcessesPanel&&) = delete;

    /// Initialize the panel (creates ProcessModel and starts sampler).
    void onAttach() override;

    /// Cleanup (stops sampler).
    void onDetach() override;

    /// Update logic (no longer needed for refresh, kept for interface compatibility).
    void onUpdate(float deltaTime) override;

    /// Render the panel.
    /// @param open Pointer to visibility flag (for window close button).
    void render(bool* open) override;

    /// Get the currently selected process PID.
    /// @return Selected PID, or -1 if none selected.
    [[nodiscard]] int32_t selectedPid() const
    {
        return m_SelectedPid;
    }

    /// Get the process count.
    [[nodiscard]] size_t processCount() const;

    /// Get the current process snapshots.
    [[nodiscard]] std::vector<Domain::ProcessSnapshot> snapshots() const;

    /// Get column settings (for persistence)
    [[nodiscard]] const ProcessColumnSettings& columnSettings() const
    {
        return m_ColumnSettings;
    }

    /// Set column settings (from loaded config)
    void setColumnSettings(const ProcessColumnSettings& settings)
    {
        m_ColumnSettings = settings;
    }

  private:
    std::unique_ptr<Domain::ProcessModel> m_ProcessModel;
    std::unique_ptr<Domain::BackgroundSampler> m_Sampler;
    int32_t m_SelectedPid = -1;

    // Column visibility
    ProcessColumnSettings m_ColumnSettings;

    // Search/filter state
    std::array<char, 256> m_SearchBuffer{};

    /// Get the number of visible columns
    [[nodiscard]] int visibleColumnCount() const;
};

} // namespace App
