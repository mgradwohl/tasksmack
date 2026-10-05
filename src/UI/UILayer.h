#pragma once

#include "Core/Layer.h"
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
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onPostRender() override;
    void onEvent(Core::Event& event) override;
    void onSDLEvent(SDL_Event* event) override;

  private:
    void beginFrame();
    void endFrame();
    static void loadAllFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale);
    static void loadFallbackFonts(FontFileCache& fontFiles, const std::filesystem::path& assetsDir, float displayScale);
    void rebuildForDisplayScaleChange();

    // Where the fonts were loaded from, kept to rebuild them at a new display scale (#943).
    std::filesystem::path m_AssetsDir;
    // Every font file's bytes, read once and shared by all the fonts made from it, at startup and
    // on each rebuild (#1170). The atlas reads them for as long as it holds those fonts, so this
    // must outlive it: ImGui's context, and with it the atlas, is destroyed in onDetach(), before
    // this member.
    FontFileCache m_FontFiles;
    // Set when SDL reports the window's display scale or display changed; checked and cleared at the
    // next frame boundary, where the fonts and style can be rebuilt between frames (#943).
    bool m_DisplayScaleCheckPending = false;

    // Per-frame render state shared between beginFrame() and endFrame()
    ImFont* m_PushedFont = nullptr; // font pushed in beginFrame, popped in endFrame
    int m_CachedPixelW = 0;         // last known framebuffer width  (avoids redundant glViewport)
    int m_CachedPixelH = 0;         // last known framebuffer height (avoids redundant glViewport)
};

} // namespace UI
