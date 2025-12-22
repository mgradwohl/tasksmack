#pragma once

#include "Core/Layer.h"
#include "UI/IconLoader.h"

#include <string>

namespace App
{

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

    static auto instance() -> AboutLayer*;
    void requestOpen();

  private:
    void renderAboutDialog();
    void loadIcon();
    void openUrl(const std::string& url) const;

    bool m_OpenRequested = false;
    UI::Texture m_Icon;

    static AboutLayer* s_Instance;
};

} // namespace App
