#include "ShellHost.h"

#include "Core/Application.h"
#include "Core/Event.h"
#include "Core/WindowGeometry.h"
#include "ShellLayer.h"
#include "TitleBarLayer.h"
#include "WindowOverride.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace
{

class ApplicationShellHost final : public IShellHost
{
  public:
    void raiseEvent(Core::Event& event) override
    {
        Core::Application::get().raiseEvent(event);
    }

    [[nodiscard]] double lastFrameIntervalSeconds() const override
    {
        return Core::Application::get().lastFrameIntervalSeconds();
    }

    [[nodiscard]] bool isBorderless() const override
    {
        return Core::Application::get().getWindow().isBorderless();
    }

    [[nodiscard]] float titleBarHeight() const override
    {
        return TitleBarLayer::height();
    }

    void requestClose() override
    {
        Core::Application::get().getWindow().requestClose();
    }

    void setWindowTitle(const std::string& title) override
    {
        Core::Application::get().getWindow().setTitle(title);
    }

    [[nodiscard]] SDL_DisplayID displayId() const override
    {
        return Core::Application::get().getWindow().getDisplayId();
    }

    [[nodiscard]] std::optional<std::pair<int, int>> usableDisplaySize() const override
    {
        return Core::Application::get().getWindow().getUsableDisplaySize();
    }

    [[nodiscard]] bool hasWindow() const override
    {
        return Core::Application::get().getWindow().getHandle() != nullptr;
    }

    void setMinimumWindowSize(int width, int height, Core::WindowGeometry::MinimumSizeChange change) override
    {
        // Logs its own failure; keeps the current size on a tiled Wayland window (#1777).
        static_cast<void>(Core::Application::get().getWindow().setMinimumSize(width, height, change));
    }

    [[nodiscard]] WindowOverride::CapturedWindow captureWindow() const override
    {
        const Core::Window& window = Core::Application::get().getWindow();
        return {
            .normal = window.getNormalGeometry(),
            .normalScale = window.getNormalGeometryScale(),
            .canPosition = Core::Window::supportsPositioning(),
            .maximized = window.isMaximized(),
        };
    }
};

} // namespace

std::unique_ptr<IShellHost> makeApplicationShellHost()
{
    return std::make_unique<ApplicationShellHost>();
}

// Here rather than in ShellLayer.cpp, so the headless tests can link ShellLayer.cpp with a fake host
// and without TitleBarLayer.cpp (#1547).
ShellLayer::ShellLayer() : ShellLayer(makeApplicationShellHost())
{}

} // namespace App
