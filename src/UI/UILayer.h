#pragma once

#include "Core/Layer.h"

#include <filesystem>

union SDL_Event;
struct ImFont; // forward-declare ImGui font type

namespace UI
{

class UILayer : public Core::Layer
{
  public:
    UILayer();
    ~UILayer() override;

    UILayer(const UILayer&) = delete;
    UILayer& operator=(const UILayer&) = delete;
    UILayer(UILayer&&) = delete;
    UILayer& operator=(UILayer&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onPostRender() override;
    void onEvent(Core::Event& event) override;
    void onSDLEvent(SDL_Event* event) override;

  private:
    void beginFrame();
    void endFrame();
    static void loadAllFonts(const std::filesystem::path& assetsDir);

    // Per-frame render state shared between beginFrame() and endFrame()
    ImFont* m_PushedFont = nullptr; // font pushed in beginFrame, popped in endFrame
    int m_CachedPixelW = 0;         // last known framebuffer width  (avoids redundant glViewport)
    int m_CachedPixelH = 0;         // last known framebuffer height (avoids redundant glViewport)
};

} // namespace UI
