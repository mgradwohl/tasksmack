#pragma once

// The OpenGL version TaskSmack's renderer needs, and the check Core::Window makes against the context
// it actually got (#1398). Pure, so it is unit-tested without a context.

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
