#include "NativeWindow.h"

#include "Core/Application.h"
#include "Core/Window.h"

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

namespace App
{

void* mainNativeWindow()
{
    try
    {
        const auto& app = Core::Application::get();
        if (!app.hasWindow())
        {
            return nullptr;
        }
        return SDL_GetPointerProperty(SDL_GetWindowProperties(app.getWindow().getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    }
    catch (...) // no Application (headless tests): nothing to belong to
    {
        return nullptr;
    }
}

} // namespace App
