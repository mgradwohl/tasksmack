#pragma once

// What a TaskSmack run as root for a user takes from SDL's environment variables (#4111, the Linux side of #4103):
// nothing. Its environment is the user's (sudo -E, or an env_keep list that keeps them), and SDL reads a hint it isn't
// given from the variable of the same name. Some name a library SDL dlopen()s, as root: SDL_OPENGL_LIBRARY (the X11
// GL loader and the EGL path Wayland uses), SDL_EGL_LIBRARY, SDL_X11_XCB_LIBRARY, SDL_VULKAN_LIBRARY. SDL3_DYNAMIC_API,
// which SDL's dynamic API reads with getenv() at its first call, static build or not, replaces SDL itself with the
// library it names. glibc's secure-execution mode doesn't apply: a sudo session isn't set-user-ID for TaskSmack.
//
// So main() removes every SDL variable from its environment (unsetenv) before its first SDL call, in a session run as
// root for a user (SessionUser::invokingUserHome(), the rule #4090 and #4104 use), and runApp() logs their names once
// logging is up, never their values. Every one, not a list of the risky ones, which would go stale as SDL adds hints;
// the hints TaskSmack sets itself, with SDL_SetHint, still apply. Plain sudo (env_reset) and pkexec clear them anyway.
//
// The logic takes the environment and the unset call as arguments, so test_LinuxSdlEnvironment.cpp covers it with a
// fake one; removeForThisProcess() works on the process's own.

#ifdef __linux__

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::SdlEnvironment
{

/// Whether @p name is a variable SDL 3 reads: "SDL_", or "SDL" then digits then "_" (SDL3_DYNAMIC_API, SDL2's older
/// names). Exact case, as Linux compares environment variable names.
[[nodiscard]] bool isSdlVariable(std::string_view name) noexcept;

/// The variables of the libraries SDL's OpenGL/EGL window pulls in that name a library or a directory of them to load
/// (#4151): Mesa's driver and GBM backend paths, libglvnd's vendor files (JSON naming the .so) and vendor name,
/// libdecor's plugins (Wayland decorations), and the Vulkan loader's drivers and layers. Under sudo, uid and euid are
/// both 0, so neither glibc's secure mode nor the libraries' own getuid() != geteuid() checks drop them. A fixed list:
/// unlike SDL's hints, these names don't change from release to release. A list can't name every library's variables,
/// and GTK also reads the user's own settings.ini, so GTK is kept out of the process instead (PRIVILEGED_SESSION_SETTINGS, #4254); its
/// module and path variables are listed too, for any other GLib reader.
inline constexpr std::array<std::string_view, 36> LOADER_VARIABLES = {
    "LIBGL_DRIVERS_PATH",
    "LIBGL_DRIVERS_DIR",
    "EGL_DRIVERS_PATH",
    "GBM_BACKENDS_PATH",
    "MESA_LOADER_DRIVER_OVERRIDE",
    "__EGL_VENDOR_LIBRARY_FILENAMES",
    "__EGL_VENDOR_LIBRARY_DIRS",
    "__GLX_VENDOR_LIBRARY_NAME",
    "LIBDECOR_PLUGIN_DIR",
    "VK_ICD_FILENAMES",
    "VK_DRIVER_FILES",
    "VK_ADD_DRIVER_FILES",
    "VK_LAYER_PATH",
    "VK_ADD_LAYER_PATH",
    "VK_IMPLICIT_LAYER_PATH",
    "VK_ADD_IMPLICIT_LAYER_PATH",
    // NVIDIA's EGL external platforms: JSON manifests whose library_path (egl-wayland, egl-gbm) libEGL_nvidia loads.
    "__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS",
    "__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES",
    // glibc's iconv modules (dlopen()ed for a charset it doesn't build in), and libX11's loadable i18n modules.
    "GCONV_PATH",
    "XLOCALEDIR",
    "XLOCALELIBDIR",
    // GTK's and GLib's modules (#4254): what a setcap session's GTK would dlopen(), and the folders it looks in.
    "GTK_MODULES",
    "GTK3_MODULES",
    "GTK_PATH",
    "GTK_EXE_PREFIX",
    "GTK_DATA_PREFIX",
    "GTK_IM_MODULE",
    "GTK_IM_MODULE_FILE",
    "GIO_EXTRA_MODULES",
    "GIO_MODULE_DIR",
    "GDK_PIXBUF_MODULEDIR",
    "GDK_PIXBUF_MODULE_FILE",
    "GSETTINGS_BACKEND",
    "GSETTINGS_SCHEMA_DIR",
    // Mesa's shader cache folders (#4285): llvmpipe maps the machine code it finds there executable.
    "MESA_SHADER_CACHE_DIR",
    "MESA_GLSL_CACHE_DIR",
};

/// What a session whose environment may be another user's sets, after both of main()'s scrubs (#4254, #4285), so no
/// library loads code from what that user controls, their own files included:
/// - SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0: SDL's Wayland window talks to the compositor itself (xdg-shell) and never
///   loads libdecor, whose GTK plugin runs gtk_init_check(). GTK loads the modules its environment and the user's own
///   ~/.config/gtk-3.0/settings.ini (gtk-modules=) name, and its setuid check compares uids and gids only, so a setcap
///   session (cap_sys_ptrace, the FAQ's install) would run them with its capabilities. TaskSmack draws its own title
///   bar there (Application). cmake/Dependencies.cmake makes sure SDL loads libdecor, so that the hint applies.
/// - SDL_ENABLE_GTK=0: SDL itself never initialises GTK either (SDL_CanUseGtk()).
/// - MESA_SHADER_CACHE_DISABLE=true, MESA_GLSL_CACHE_DISABLE=1 (older Mesa): Mesa's shader cache is the user's
///   (their HOME or XDG_CACHE_HOME), and llvmpipe maps the machine code it finds there executable (#4285).
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> PRIVILEGED_SESSION_SETTINGS = {
    {
        {"SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0"},
        {"SDL_ENABLE_GTK", "0"},
        {"MESA_SHADER_CACHE_DISABLE", "true"},
        {"MESA_GLSL_CACHE_DISABLE", "1"},
    },
};

/// libglvnd's per-screen vendor override, __GLX_FORCE_VENDOR_LIBRARY_<n>, matched by its prefix.
inline constexpr std::string_view FORCE_VENDOR_PREFIX = "__GLX_FORCE_VENDOR_LIBRARY_";

/// Whether @p name is one of LOADER_VARIABLES, or starts with FORCE_VENDOR_PREFIX. Exact case.
[[nodiscard]] bool isLoaderVariable(std::string_view name) noexcept;

/// The names of the SDL variables, and the loader variables (#4151), in @p environment ("NAME=value" strings, null-terminated, as environ
/// is), in order. An entry with no '=' is taken whole as a name.
[[nodiscard]] std::vector<std::string> sdlVariableNames(const char* const* environment);

/// Removes each SDL variable and loader variable in @p environment with @p unset. @return The names removed (a failed unset is left out).
/// The names are collected first, so unsetting while walking environ can't skip one.
[[nodiscard]] std::vector<std::string> removeSdlVariables(const char* const* environment,
                                                          const std::function<bool(const std::string&)>& unset);

/// Sets each of PRIVILEGED_SESSION_SETTINGS with @p set (#4254, #4285). @return Whether every one was set.
[[nodiscard]] bool keepPrivilegedLoadersOut(const std::function<bool(const std::string&, const std::string&)>& set);

/// main()'s call, before any SDL call: when @p remove, removes every SDL and loader variable from this process's
/// environment. Not thread-safe (unsetenv), so first thing in main().
void removeForThisProcess(bool remove);

/// main()'s call after this scrub and RootEnvironment's allow-list (which would drop them): when @p apply,
/// keepPrivilegedLoadersOut() on this process's environment, and TaskSmack stops if one can't be set. Not
/// thread-safe (setenv).
void keepPrivilegedLoadersOutOfThisProcess(bool apply);

/// The names removeForThisProcess() removed, for runApp() to log.
[[nodiscard]] const std::vector<std::string>& removedFromThisProcess() noexcept;

/// This process's environment ("NAME=value" strings) as it started, before removeForThisProcess() and
/// keepPrivilegedLoadersOutOfThisProcess() changed it (#4651); empty when they didn't run. For the programs a setcap
/// session starts as its own user, who hold none of its capabilities (SessionUser::childMayGetStartingEnvironment()):
/// the scrub keeps the user's GTK, GIO and Mesa settings out of TaskSmack, not out of their own programs.
[[nodiscard]] const std::vector<std::string>& environmentBeforeScrub() noexcept;

} // namespace Platform::SdlEnvironment

#endif
