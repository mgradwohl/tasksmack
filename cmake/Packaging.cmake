# CPack configuration for packaging (ZIP/TGZ everywhere, NSIS on Windows,
# DragNDrop on macOS, DEB/RPM on Linux when tooling is available).
# Include LAST, after install rules and subdirectories.

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} - TaskSmack")
set(CPACK_PACKAGE_DESCRIPTION "TaskSmack - Modern cross-platform system monitor")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mgradwohl/tasksmack")
set(CPACK_PACKAGE_VENDOR "TaskSmack Contributors")
set(CPACK_PACKAGE_CONTACT "mgradwohl@users.noreply.github.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.ico")

# Package only the default install component, i.e. TaskSmack's own install() rules in
# cmake/InstallRules.cmake. The FetchContent dependencies (cmake/Dependencies.cmake), tests/
# (googletest, rapidcheck) and benchmarks/ install into their own third_party/tests/benchmarks
# components, so nothing a dependency installs ends up in a package (#1398).
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};Unspecified;/")
# The .deb and .rpm add the privileged helper and its polkit policy (the "helper" component,
# cmake/InstallRules.cmake; #2307): cmake/CPackProjectConfig.cmake.in widens the list above for them.
# The .zip and the NSIS installer add TaskSmackHelper.exe (the "windows_helper" component; #2550) only
# for a build that is Authenticode-signed (#2302), and then check both executables' signatures first.
if(WIN32 AND TARGET TaskSmackHelper AND TASKSMACK_PACKAGE_WINDOWS_HELPER)
    set(TASKSMACK_PACKAGES_WINDOWS_HELPER ON)
else()
    set(TASKSMACK_PACKAGES_WINDOWS_HELPER OFF)
endif()
configure_file(${CMAKE_SOURCE_DIR}/cmake/CPackProjectConfig.cmake.in ${CMAKE_BINARY_DIR}/CPackProjectConfig.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/CPackProjectConfig.cmake")

# Package file names (#2214): x64 keeps CPack's default system name (win64 / Linux), so the
# release asset names existing download links point at don't change; an ARM64 build is named
# windows-arm64 / linux-aarch64, so its .zip, .tar.gz and .deb can sit beside the x64 ones on one
# release. Decided from what the compiler targets rather than CMAKE_SYSTEM_PROCESSOR, which on
# Windows comes from the environment of the CMake process (AMD64 when an x64 CMake runs emulated on
# an ARM64 machine). Compiled only, never linked, so the release link flags play no part.
block(SCOPE_FOR VARIABLES PROPAGATE TASKSMACK_PACKAGE_TARGETS_ARM64)
    include(CheckCXXSourceCompiles)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_cxx_source_compiles(
        "#if !defined(__aarch64__) && !defined(_M_ARM64)
#error not ARM64
#endif
int main() { return 0; }"
        TASKSMACK_PACKAGE_TARGETS_ARM64)
endblock()
if(TASKSMACK_PACKAGE_TARGETS_ARM64)
    if(WIN32)
        set(CPACK_SYSTEM_NAME "windows-arm64")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(CPACK_SYSTEM_NAME "linux-aarch64")
    endif()
endif()

# Output directory for packages
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")

# Archive generators (cross-platform)
set(CPACK_GENERATOR "ZIP;TGZ")

# Platform-specific generators
if(WIN32)
    list(APPEND CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH ON)
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.ico")
    # Per machine, in Program Files, by an administrator (#2550): the template's RequestExecutionLevel
    # admin, and the 64-bit Program Files named here rather than left to CPack's default. Only an install
    # TaskSmack's user can't change may use TaskSmackHelper.exe (#2572), so a copy in a folder the user
    # picks instead, such as one under their profile, runs but can't act as an administrator.
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
    # A Start menu shortcut to the installed TaskSmack.exe, which is at the install's root (InstallRules.cmake).
    set(CPACK_PACKAGE_EXECUTABLES "${PROJECT_NAME};${PROJECT_NAME}")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "${PROJECT_NAME}.exe")
    set(CPACK_NSIS_DISPLAY_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_HELP_LINK "https://github.com/mgradwohl/tasksmack")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/mgradwohl/tasksmack")
    # How the uninstaller is built so it can be signed (#2302), and how release.yml builds the installer
    # it publishes from the signed .zip: cmake/CPackProjectConfig.cmake.in. Needs NSIS 3.08 or later.
elseif(APPLE)
    list(APPEND CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    # Check for dpkg (Debian/Ubuntu)
    find_program(DPKG_PROGRAM dpkg)
    if(DPKG_PROGRAM)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
        set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
        set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
        # Depends on the libraries the binary links (libc6, libgcc-s1, libx11-6, libgl1/
        # libopengl0, ...) come from dpkg-shlibdeps. libc++/libc++abi are linked statically in
        # Release (TASKSMACK_STATIC_LIBCXX, #1398), and SDL3 and FreeType are static too.
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
        # dpkg-shlibdeps can't see what SDL3 dlopen()s at run time. X11 needs libX11 and libXext,
        # and the GL context is created through GLX or EGL, so those are hard dependencies. The
        # rest SDL3 uses when present: the other X11 extensions, Wayland, and Mesa's drivers.
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxext6, libgl1, libegl1")
        set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
            "libxcursor1, libxi6, libxrandr2, libxfixes3, libxkbcommon0, libwayland-client0, libwayland-cursor0, libwayland-egl1, libdecor-0-0, libgl1-mesa-dri, libegl-mesa0, pkexec | policykit-1")
    endif()
    # Check for rpmbuild (Red Hat/Fedora)
    find_program(RPMBUILD_PROGRAM rpmbuild)
    if(RPMBUILD_PROGRAM)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

include(CPack)
