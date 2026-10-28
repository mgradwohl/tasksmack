#pragma once

#include "Core/OpenGLRequirement.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <string_view>

namespace TestSupport
{

// True when the environment variable @p name is exactly "1".
[[maybe_unused]] inline bool envFlagSet(const char* name)
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    _dupenv_s(&value, &len, name);
    const bool set = (value != nullptr && std::string_view(value) == "1");
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory, cppcoreguidelines-no-malloc) - _dupenv_s allocates with malloc; must free with free()
    free(value);
    return set;
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - read-only env access during single-threaded test setup
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
#endif
}

// True when the environment promises a GL-capable display (TASKSMACK_REQUIRE_DISPLAY=1), as CI
// does: Linux runs the tests under Xvfb + Mesa, Windows on the runner's desktop with Mesa's
// llvmpipe beside the test executables (#2134). There, "no display" means the CI setup broke, and
// the display-dependent suites must fail rather than quietly skip (#1132).
[[maybe_unused]] inline bool displayRequired()
{
    return envFlagSet("TASKSMACK_REQUIRE_DISPLAY");
}

// Passes a display probe's result through, recording a test failure first when the probe found
// no display although displayRequired() promised one. The caller still skips, but the test is
// reported as failed, not skipped.
[[maybe_unused]] inline bool enforceDisplayRequirement(bool displayAvailable)
{
    if (!displayAvailable && displayRequired())
    {
        ADD_FAILURE() << "TASKSMACK_REQUIRE_DISPLAY=1, but no GL-capable display was found";
    }
    return displayAvailable;
}

// Whether the context SDL created is really OpenGL 3.3 or newer, by the same rule Core::Window applies.
// A non-null context is not enough: on Windows, when the driver is Microsoft's GDI Generic (no GPU
// driver, no Mesa llvmpipe beside the executable), SDL hands back a legacy OpenGL 1.1 context instead of
// failing, and Core::Window then refuses it, so the suites would fail instead of skipping (#2257).
// glGetString is read through SDL_GL_GetProcAddress, since this header must not include GLAD.
[[maybe_unused]] inline bool contextMeetsRequiredGLVersion(SDL_Window* window, SDL_GLContext context)
{
    if (!SDL_GL_MakeCurrent(window, context))
    {
        return false;
    }
    // 64-bit builds only (#2214): no __stdcall, which only 32-bit x86 Windows distinguished.
    using GetStringFn = const unsigned char* (*)(unsigned int);
    constexpr unsigned int GL_VERSION_ENUM = 0x1F02; // GL_VERSION
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - SDL returns GL entry points as a generic function pointer
    const auto getString = reinterpret_cast<GetStringFn>(SDL_GL_GetProcAddress("glGetString"));
    const unsigned char* version = getString != nullptr ? getString(GL_VERSION_ENUM) : nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - GLubyte* version string is plain ASCII
    const bool capable = Core::glVersionStringMeetsRequirement(reinterpret_cast<const char*>(version));
    SDL_GL_MakeCurrent(window, nullptr);
    return capable;
}

// Returns true if SDL can initialize video AND create an OpenGL 3.3 core context. On every platform:
// a Windows machine without GL 3.3 (a VM, a basic display adapter) must skip the display-dependent
// suites rather than fail them, since their construction failures are fatal once a display is
// detected (#1132).
// Uses the same SDL_GL attributes as Core::Window so a display that only supports
// a default/legacy context is correctly rejected, and then checks the version of the context it
// actually got (contextMeetsRequiredGLVersion), since SDL may return an older one.
// Calls SDL_Init / SDL_Quit internally; do not call while SDL is already initialized.
[[maybe_unused]] inline bool probeGLCapability()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        return false;
    }
    // Match the GL attributes set by Core::Window before creating its context.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
    // And the same framebuffer (src/Core/Window.cpp): double-buffered, no depth or stencil. SDL's
    // default asks for a depth buffer, which a driver could lack while supporting the app's own
    // framebuffer, making the probe skip (or, with TASKSMACK_REQUIRE_DISPLAY=1, fail) wrongly.
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    bool glCapable = false;
    SDL_Window* testWin = SDL_CreateWindow("gl_probe", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (testWin != nullptr)
    {
        SDL_GLContext ctx = SDL_GL_CreateContext(testWin);
        if (ctx != nullptr)
        {
            glCapable = contextMeetsRequiredGLVersion(testWin, ctx);
            SDL_GL_DestroyContext(ctx);
        }
        SDL_DestroyWindow(testWin);
    }
    SDL_Quit();
    return glCapable;
}

#ifdef _WIN32
// Windows' display check for the suites that open real windows. A Windows session always has a
// desktop, so the question is only whether SDL can create a GL 3.3 core context on it: a real GPU
// driver, or Mesa's llvmpipe beside the test executable as CI provides (#2134). A machine with
// neither (a VM, a basic display adapter, a CI job without llvmpipe) skips. There is no CI=true
// bypass any more; TASKSMACK_SKIP_DISPLAY_TESTS=1 is the explicit opt-out, for a local run that
// shouldn't open windows on a desktop someone is using.
[[maybe_unused]] inline bool detectWindowsDisplay()
{
    return !envFlagSet("TASKSMACK_SKIP_DISPLAY_TESTS") && probeGLCapability();
}
#endif

[[maybe_unused]] inline bool tryEnableOffscreenVideoDriver()
{
#ifdef _WIN32
    return false;
#else
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    // All env-var access below occurs during single-threaded test SetUp.

    const char* videoDriver = std::getenv("SDL_VIDEODRIVER");
    if (videoDriver != nullptr && videoDriver[0] != '\0')
    {
        // The offscreen driver is the intended headless fallback; trust it immediately.
        // Core::Window tests already handle GL context failure with GTEST_SKIP() via
        // isOffscreenVideoDriver(), so no GL capability probe is needed here.
        if (std::string_view(videoDriver) == "offscreen")
        {
            return true;
        }

        // For any other pre-configured driver (e.g. x11, wayland, dummy), verify GL
        // capability. Drivers such as 'dummy' pass SDL_Init(SDL_INIT_VIDEO) and can
        // load libGL, but cannot create GL contexts; returning true for such a driver
        // would cause Window construction to hit FAIL() instead of GTEST_SKIP().
        // Clear the variable and fall through to offscreen if the probe fails.
        if (probeGLCapability())
        {
            return true;
        }
        unsetenv("SDL_VIDEODRIVER");
    }

    if (setenv("SDL_VIDEODRIVER", "offscreen", 1) != 0)
    {
        return false;
    }
    [[maybe_unused]] const int audioSetResult = setenv("SDL_AUDIODRIVER", "dummy", 1);

    // SDL3 snapshots the process environment on first use; subsequent setenv() calls
    // may not be visible to SDL if it already cached an earlier snapshot. Set the
    // hint at OVERRIDE priority to guarantee SDL uses the offscreen driver regardless.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    // Verify the offscreen driver can actually initialize the SDL video subsystem.
    // The Application constructor's catch block cleans up the singleton on failure,
    // so an SDL initialization failure here is safe to detect and recover from.
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        // The offscreen driver is not functional; clear both the env var and the
        // SDL hint override so later SDL initializations are not forced to the
        // non-functional offscreen driver.
        unsetenv("SDL_VIDEODRIVER");
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        return false;
    }
    SDL_Quit();
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return true;
#endif
}

} // namespace TestSupport
