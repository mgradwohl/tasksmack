#pragma once

#include "Core/Layer.h"

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

  private:
    static void beginFrame();
    static void endFrame();
    static void loadAllFonts();
};

} // namespace UI
