#pragma once

// What ShellLayer needs from the running application and its window (#1547): its events, the frame's
// real duration, the title bar's height, a close request, the window's minimum size and the geometry
// saved on exit. The seam a headless test fakes, so the shell's frame can be drawn, and its settings
// saved, without SDL, a GL context or an Application.

#include "Core/Event.h"
#include "Core/WindowGeometry.h"
#include "WindowOverride.h"

#include <SDL3/SDL_video.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

class IShellHost
{
  public:
    IShellHost() = default;
    virtual ~IShellHost() = default;

    IShellHost(const IShellHost&) = delete;
    IShellHost& operator=(const IShellHost&) = delete;
    IShellHost(IShellHost&&) = delete;
    IShellHost& operator=(IShellHost&&) = delete;

    /// Core::Application::raiseEvent(): to every layer, top down.
    virtual void raiseEvent(Core::Event& event) = 0;
    /// Core::Application::lastFrameIntervalSeconds(): the last frame's real duration, uncapped.
    [[nodiscard]] virtual double lastFrameIntervalSeconds() const = 0;
    /// Whether the custom title bar replaces the native decorations (#745).
    [[nodiscard]] virtual bool isBorderless() const = 0;
    /// The custom title bar's height (TitleBarLayer::height()); only asked for when borderless.
    [[nodiscard]] virtual float titleBarHeight() const = 0;
    /// The title bar Close button's path: a close request, vetoable.
    virtual void requestClose() = 0;
    /// Core::Window::setTitle(): the OS window title, "TaskSmack (Administrator)" in such a session (#2728).
    virtual void setWindowTitle(const std::string& title) = 0;
    /// The display the window is on, to notice a move to another (#1207).
    [[nodiscard]] virtual SDL_DisplayID displayId() const = 0;
    /// That display's usable size in pixels, when known.
    [[nodiscard]] virtual std::optional<std::pair<int, int>> usableDisplaySize() const = 0;
    /// Whether there is a window to set a minimum size on.
    [[nodiscard]] virtual bool hasWindow() const = 0;
    /// Core::Window::setMinimumSize(): @p change says whether a smaller window grows to it (#1945). A
    /// refusal is logged, and the next scale or display change tries again.
    virtual void setMinimumWindowSize(int width, int height, Core::WindowGeometry::MinimumSizeChange change) = 0;
    /// The window's normal (restored) geometry, its scale, and whether it is maximized, for the
    /// settings saved on exit (#1121, #1168).
    [[nodiscard]] virtual WindowOverride::CapturedWindow captureWindow() const = 0;
};

/// The real host: Core::Application, its window and TitleBarLayer::height().
[[nodiscard]] std::unique_ptr<IShellHost> makeApplicationShellHost();

} // namespace App
