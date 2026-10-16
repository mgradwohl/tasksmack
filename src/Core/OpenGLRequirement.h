#pragma once

// The OpenGL version TaskSmack's renderer needs, and the check Core::Window makes against the context
// it actually got (#1398). Pure, so it is unit-tested without a context.

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace Core
{

/// The OpenGL version Core::Window requests (a core profile) and the ImGui renderer's GLSL 330 shaders need.
constexpr int REQUIRED_GL_MAJOR = 3;
constexpr int REQUIRED_GL_MINOR = 3;

/// Whether a context of OpenGL major.minor can run TaskSmack's renderer.
///
/// SDL does not guarantee the version it was asked for: on Windows, when the driver has no
/// WGL_ARB_create_context (Microsoft's "GDI Generic" OpenGL 1.1, the only OpenGL on a PC or VM with no
/// GPU driver, e.g. a GitHub-hosted runner), SDL_GL_CreateContext() returns the legacy context it
/// created first, and GLAD loads that successfully too. Every OpenGL 2.0+ entry point is then null,
/// and the first one called crashes.
[[nodiscard]] constexpr bool meetsRequiredGLVersion(const int major, const int minor) noexcept
{
    return major > REQUIRED_GL_MAJOR || (major == REQUIRED_GL_MAJOR && minor >= REQUIRED_GL_MINOR);
}

/// Whether a glGetString(GL_VERSION) string names a context that can run TaskSmack's renderer. The string
/// starts with "major.minor" ("3.3 (Core Profile) Mesa 26.2.4", "4.6.0 NVIDIA 560.94", GDI Generic's
/// "1.1.0"); anything that does not (empty, unparseable) is rejected. For a caller that has a
/// context but not GLAD's parsed version, such as the tests' display probe (#2257).
[[nodiscard]] constexpr bool glVersionStringMeetsRequirement(const std::string_view version) noexcept
{
    // Reads the decimal number at pos, advancing past it; -1 if there is none.
    const auto readNumber = [version](std::size_t& pos) noexcept
    {
        constexpr int MAX_COMPONENT = 1000; // no real version comes close; stops overflow on garbage
        int value = -1;
        while (pos < version.size() && version[pos] >= '0' && version[pos] <= '9' && value < MAX_COMPONENT)
        {
            value = (value < 0 ? 0 : value * 10) + (version[pos] - '0');
            ++pos;
        }
        return value;
    };

    std::size_t pos = 0;
    const int major = readNumber(pos);
    if (major < 0 || pos >= version.size() || version[pos] != '.')
    {
        return false;
    }
    ++pos;
    const int minor = readNumber(pos);
    return minor >= 0 && meetsRequiredGLVersion(major, minor);
}

/// The overload for glGetString()'s raw result, which may be null when there is no current context.
[[nodiscard]] inline bool glVersionStringMeetsRequirement(const char* version) noexcept
{
    return version != nullptr && glVersionStringMeetsRequirement(std::string_view{version});
}

/// The error TaskSmack stops with (shown in a message box on Windows) when the context is too old.
/// version, renderer and vendor are the context's glGetString() values.
[[nodiscard]] inline std::string
unsupportedGLVersionMessage(const std::string_view version, const std::string_view renderer, const std::string_view vendor)
{
    return std::format("TaskSmack needs OpenGL {}.{} or newer, but the graphics driver provides only OpenGL {} ({}, {}). "
                       "Install or update the graphics driver for this PC's GPU.",
                       REQUIRED_GL_MAJOR,
                       REQUIRED_GL_MINOR,
                       version,
                       renderer,
                       vendor);
}

} // namespace Core
