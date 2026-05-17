#pragma once

#include "Core/Layer.h"

#include <filesystem>

union SDL_Event;

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
    static void beginFrame();
    static void endFrame();
    static void loadAllFonts(const std::filesystem::path& assetsDir);
};

} // namespace UI
