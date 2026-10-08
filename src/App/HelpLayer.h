#pragma once

#include "App/HelpWindow.h"
#include "Core/Layer.h"

namespace App
{

/// Help window layer (#172). Opened by raising Core::OpenHelpEvent (F1, the title bar's and status
/// bar's ? buttons); the window itself is App::HelpWindow. Its "About TaskSmack..." link raises
/// Core::OpenAboutEvent for AboutLayer.
/// Thread safety: All layer lifecycle methods (onAttach/onDetach/onUpdate/onRender)
/// are guaranteed to be called from the main thread only, as required by SDL and ImGui.
class HelpLayer : public Core::Layer
{
  public:
    HelpLayer();
    ~HelpLayer() override;

    HelpLayer(const HelpLayer&) = delete;
    HelpLayer& operator=(const HelpLayer&) = delete;
    HelpLayer(HelpLayer&&) = delete;
    HelpLayer& operator=(HelpLayer&&) = delete;

    void onAttach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    HelpWindow::State m_State;
};

} // namespace App
