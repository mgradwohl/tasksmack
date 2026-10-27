#pragma once

#include "App/BackgroundOpen.h"
#include "App/SettingsDialog.h"
#include "Core/Event.h"
#include "Core/Layer.h"

#include <filesystem>
#include <functional>

namespace App
{

/// Settings dialog layer. Opened by raising Core::OpenSettingsEvent (the title bar's gear, F2, the status
/// bar) or by TASKSMACK_OPEN=settings at startup, and closed by Core::CloseDialogEvent as Cancel closes it
/// (#2827).
/// Thread safety: All layer lifecycle methods (onAttach/onDetach/onUpdate/onRender)
/// are guaranteed to be called from the main thread only, as required by SDL and ImGui.
class SettingsLayer : public Core::Layer
{
  public:
    SettingsLayer();

    /// Test seam (#1395): the events the layer raises (refresh rate, history length, About) go to
    /// @p raiseEvent instead of Core::Application, the Edit Config and Open Themes Folder buttons hand
    /// their path to @p openPath instead of the system handler, and the themes folder is made under
    /// @p userConfigDir instead of the Application's config directory. Lets the layer run headless.
    /// @p openPath is called on the layer's open worker (#3155), never on the UI thread.
    SettingsLayer(std::function<void(Core::Event&)> raiseEvent,
                  std::function<bool(const std::filesystem::path&)> openPath,
                  std::function<std::filesystem::path()> userConfigDir);
    ~SettingsLayer() override;

    SettingsLayer(const SettingsLayer&) = delete;
    SettingsLayer& operator=(const SettingsLayer&) = delete;
    SettingsLayer(SettingsLayer&&) = delete;
    SettingsLayer& operator=(SettingsLayer&&) = delete;

    void onAttach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

    /// Waits for an Edit Config File or Open Themes Folder still being opened on the worker, and takes
    /// it in (tests: what the opener was handed is then safe to read).
    void waitForPendingOpen();

  private:
    void requestOpen();
    void applySettings();
    void raiseEvent(Core::Event& event) const;
    /// Starts @p prepare, then the open of @p path, on the open worker (#3155); refused while one runs.
    void startOpen(std::function<void()> prepare, std::filesystem::path path);
    [[nodiscard]] std::filesystem::path userThemesDir() const;

    SettingsDialog::State m_State;
    std::function<void(Core::Event&)> m_RaiseEvent;               ///< Set by the test seam only; else the Application
    std::function<bool(const std::filesystem::path&)> m_OpenPath; ///< Set by the test seam only; else PlatformOpen
    std::function<std::filesystem::path()> m_UserConfigDir;       ///< Set by the test seam only; else the Application's
    // The buttons' open, and what it needs on disk first, on a worker (#3155): the buttons are disabled
    // until it is done. Last, so it is destroyed (waited for) first: its job holds a copy of the opener.
    BackgroundOpen m_Open;
};

} // namespace App
