#pragma once

#include "App/BackgroundOpen.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/IconLoader.h"

namespace App
{

/// About dialog layer. Opened by raising Core::OpenAboutEvent and closed by Core::CloseDialogEvent
/// (#2827); the dialog itself is App::AboutDialog.
/// Thread safety: All layer lifecycle methods (onAttach/onDetach/onUpdate/onRender)
/// are guaranteed to be called from the main thread only, as required by SDL and ImGui.
class AboutLayer : public Core::Layer
{
  public:
    AboutLayer();
    ~AboutLayer() override;

    AboutLayer(const AboutLayer&) = delete;
    AboutLayer& operator=(const AboutLayer&) = delete;
    AboutLayer(AboutLayer&&) = delete;
    AboutLayer& operator=(AboutLayer&&) = delete;

    void onAttach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void requestOpen();
    void loadIcon();

    bool m_OpenRequested = false;
    bool m_CloseRequested = false;
    UI::Texture m_Icon;
    // A link's open, on a worker (#3155): the links are disabled until it is done. Destroying the
    // layer waits for it.
    BackgroundOpen m_Open;
};

} // namespace App
