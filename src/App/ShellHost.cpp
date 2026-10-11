#include "ShellHost.h"

#include "Core/Application.h"
#include "Core/Event.h"
#include "ShellLayer.h"
#include "TitleBarLayer.h"

#include <SDL3/SDL_error.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <optional>
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

    void setMinimumWindowSize(int width, int height) override
    {
        if (SDL_Window* window = Core::Application::get().getWindow().getHandle();
            window != nullptr && !SDL_SetWindowMinimumSize(window, width, height))
        {
            spdlog::warn("SDL_SetWindowMinimumSize({}, {}) failed: {}", width, height, SDL_GetError());
        }
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
