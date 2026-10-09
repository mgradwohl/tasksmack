#pragma once

// What Core knows about the graphics stack TaskSmack itself runs on (#1519): its OpenGL context, captured
// once when the context is created, and SDL's view of the displays. Plain data, so the App layer can show
// it on the System Information page without calling OpenGL or SDL.

#include <string>
#include <vector>

namespace Core
{

/// One display as SDL sees it.
struct DisplayInfo
{
    std::string name; ///< SDL_GetDisplayName
    int x = 0;        ///< SDL_GetDisplayBounds, in window coordinates (pixels on Windows and X11)
    int y = 0;
    int width = 0;
    int height = 0;
    int pixelWidth = 0; ///< The current mode in pixels; 0 when unknown
    int pixelHeight = 0;
    double refreshHz = 0.0;    ///< 0 when unknown
    float contentScale = 0.0F; ///< 1.5 at 150 %; 0 when unknown
    bool primary = false;
    bool hdrEnabled = false; ///< SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN
};

struct GraphicsHostInfo
{
    std::string glVendor; ///< glGetString(GL_VENDOR); empty before the context exists
    std::string glRenderer;
    std::string glVersion;
    std::string videoDriver; ///< SDL's video driver: "windows", "x11", "wayland"
    std::vector<DisplayInfo> displays;
};

} // namespace Core
