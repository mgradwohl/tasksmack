#pragma once

#include "Core/Layer.h"
#include "UI/IconLoader.h"

#include <memory>
#include <string>

namespace App
{

/// About dialog layer (singleton).
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
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

    static auto instance() -> AboutLayer*;
    static void setInstance(std::unique_ptr<AboutLayer> layer);
    void requestOpen();

  private:
    void renderAboutDialog();
    void loadIcon();
    static void openUrl(const std::string& url);

    bool m_OpenRequested = false;
    UI::Texture m_Icon;

    // Singleton instance managed by std::unique_ptr (set by setAboutLayerInstance)
    // Note: Using a global instead of static member allows proper cleanup with RAII semantics
    static std::unique_ptr<AboutLayer> s_Instance;
};

} // namespace App

/// Set the global
