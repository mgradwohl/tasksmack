#include "X11WindowManager.h"

#include <SDL3/SDL.h>

#if defined(__linux__)
#include <cstddef>
#include <span>

#include <dlfcn.h>

// Xlib's Display, declared at global scope like <X11/Xlib.h> does (`typedef struct _XDisplay Display`),
// so the function-pointer types below match Xlib's exported functions exactly: calling through an
// incompatible function-pointer type is undefined behavior and fails indirect-call CFI. It must be
// outside every namespace to be the same type as Xlib's ::_XDisplay.
struct _XDisplay; // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) -- Xlib's own name
#endif

namespace Core::X11WindowManager
{

#if defined(__linux__)

namespace
{

// Xlib's types and entry points, declared here rather than through <X11/Xlib.h>: its macros (None,
// Bool, Status, True...) would leak into this file, and the functions are looked up in the libX11
// SDL has already loaded for its X11 video driver, so TaskSmack gains no link dependency on it.
using XAtom = unsigned long;          // Atom (an XID)
using XWindowId = unsigned long;      // Window (an XID)
constexpr XAtom X_ATOM_TYPE_ATOM = 4; // XA_ATOM
constexpr int X_SUCCESS = 0;          // Success
constexpr int X_TRUE = 1;
constexpr int X_FALSE = 0;
constexpr int FORMAT_32 = 32;
// More atoms than any window manager lists (GNOME's Mutter lists about 70).
constexpr long MAX_SUPPORTED_ATOMS = 4096;

using XInternAtomFn = XAtom (*)(::_XDisplay* display, const char* name, int onlyIfExists);
using XDefaultRootWindowFn = XWindowId (*)(::_XDisplay* display);
using XRootWindowFn = XWindowId (*)(::_XDisplay* display, int screenNumber);
using XGetWindowPropertyFn = int (*)(::_XDisplay* display,
                                     XWindowId window,
                                     XAtom property,
                                     long offset,
                                     long length,
                                     int deleteProperty,
                                     XAtom requestedType,
                                     XAtom* actualType,
                                     int* actualFormat,
                                     unsigned long* itemCount,
                                     unsigned long* bytesAfter,
                                     unsigned char** data);
using XFreeFn = int (*)(void* data);

/// dlsym() into the already-loaded libX11, cast to `Fn`.
template<typename Fn> [[nodiscard]] Fn lookup(void* library, const char* name) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- dlsym returns void* by POSIX definition
    return reinterpret_cast<Fn>(dlsym(library, name));
}

} // namespace

bool supportsEwmhMaximize(SDL_Window* window) noexcept
{
    if (window == nullptr)
    {
        return false;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    auto* display = static_cast<::_XDisplay*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    if (display == nullptr)
    {
        return false; // Not an X11 window
    }
    // EWMH root properties are per screen: read the root of the screen this window is on, the one SDL
    // sends its maximize request to, not the display's default screen.
    const auto screenNumber = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_SCREEN_NUMBER, -1));

    // RTLD_NOLOAD: only the copy SDL loaded (or was linked against); never load a second one.
    void* library = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (library == nullptr)
    {
        return false; // libX11 not loaded: assume no EWMH maximize
    }

    bool supported = false;
    const auto internAtom = lookup<XInternAtomFn>(library, "XInternAtom");
    const auto defaultRootWindow = lookup<XDefaultRootWindowFn>(library, "XDefaultRootWindow");
    const auto rootWindow = lookup<XRootWindowFn>(library, "XRootWindow");
    const auto getWindowProperty = lookup<XGetWindowPropertyFn>(library, "XGetWindowProperty");
    const auto xFree = lookup<XFreeFn>(library, "XFree");
    if (internAtom != nullptr && defaultRootWindow != nullptr && getWindowProperty != nullptr && xFree != nullptr)
    {
        // onlyIfExists: an atom no client ever interned can't be in anyone's _NET_SUPPORTED list.
        const XAtom netSupported = internAtom(display, "_NET_SUPPORTED", X_TRUE);
        const XAtom maximizedVert = internAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", X_TRUE);
        const XAtom maximizedHorz = internAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", X_TRUE);
        XAtom actualType = 0;
        int actualFormat = 0;
        unsigned long itemCount = 0;
        unsigned long bytesAfter = 0;
        unsigned char* data = nullptr;
        const XWindowId root =
            (screenNumber >= 0 && rootWindow != nullptr) ? rootWindow(display, screenNumber) : defaultRootWindow(display);
        if (netSupported != 0 &&
            getWindowProperty(display,
                              root,
                              netSupported,
                              0,
                              MAX_SUPPORTED_ATOMS,
                              X_FALSE,
                              X_ATOM_TYPE_ATOM,
                              &actualType,
                              &actualFormat,
                              &itemCount,
                              &bytesAfter,
                              &data) == X_SUCCESS &&
            data != nullptr)
        {
            if (actualType == X_ATOM_TYPE_ATOM && actualFormat == FORMAT_32)
            {
                // Xlib returns 32-bit-format items as an array of C longs (here atoms, unsigned long).
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- Xlib's documented format-32 layout
                const std::span<const unsigned long> atoms(reinterpret_cast<const unsigned long*>(data), itemCount);
                supported = supportedListHasMaximize(atoms, maximizedVert, maximizedHorz);
            }
            xFree(data);
        }
    }
    dlclose(library); // Drops only the reference RTLD_NOLOAD added
    return supported;
}

#else

bool supportsEwmhMaximize(SDL_Window* /*window*/) noexcept
{
    return false;
}

#endif

} // namespace Core::X11WindowManager
