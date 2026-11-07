#pragma once

#include "Core/Layer.h"
#include "UI/DpiScale.h"
#include "UI/FontFileCache.h"

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
    void onRender() override;
    void onPostRender() override;
    void onEvent(Core::Event& event) override;
    void onSDLEvent(SDL_Event* event) override;
    void onGraphicsContextLost() override;
    void onGraphicsContextRestored() override;

  private:
    void beginFrame();
    void endFrame();
    void rebuildForDisplayScaleChange();
    /// Clears the atlas and builds the fonts again at @p scale and Theme's Font size setting, falling back to
    /// ImGui's built-in font if that throws. Between frames only (beginFrame(), before NewFrame()).
    void rebuildFonts(float scale);

    // Where the fonts were loaded from, kept to rebuild them at a new display scale (#943).
    std::filesystem::path m_AssetsDir;
    // Every font file's bytes, read once and shared by all the fonts made from it, at startup and
    // on each rebuild (#1170). The atlas reads them for as long as it holds those fonts, so this
    // must outlive it: ImGui's context, and with it the atlas, is destroyed in onDetach(), before
    // this member.
    FontFileCache m_FontFiles;
    // Requested when SDL reports the window's display scale or display changed, or the window was
    // restored; taken at the next frame boundary the window isn't minimized at, where the fonts and
    // style can be rebuilt between frames (#943, #3150).
    DisplayScaleCheck m_DisplayScaleCheck;
    int m_AppliedScalePercent = 0; // The Interface scale setting the fonts were last built for (#3376)

    // Per-frame render state shared between beginFrame() and endFrame()
    ImFont* m_PushedFont = nullptr; // font pushed in beginFrame, popped in endFrame
    int m_CachedPixelW = 0;         // last known framebuffer width  (avoids redundant glViewport)
    int m_CachedPixelH = 0;         // last known framebuffer height (avoids redundant glViewport)
    // Whether the ImGui OpenGL renderer is running: false between a GPU reset's onGraphicsContextLost()
    // and onGraphicsContextRestored(), and for good if no replacement context could be made (#3484).
    bool m_RendererReady = false;
};

} // namespace UI
