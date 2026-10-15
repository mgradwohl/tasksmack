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
# cmake/InstallRules.cmake. tests/ (rapidcheck) and benchmarks/ install into their own components so
# that nothing a test or benchmark dependency installs ends up in a package (#1398).
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};Unspecified;/")

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
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\${PROJECT_NAME}.exe")
    set(CPACK_NSIS_DISPLAY_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_HELP_LINK "https://github.com/mgradwohl/tasksmack")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/mgradwohl/tasksmack")
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
            "libxcursor1, libxi6, libxrandr2, libxfixes3, libxkbcommon0, libwayland-client0, libwayland-cursor0, libwayland-egl1, libdecor-0-0, libgl1-mesa-dri, libegl-mesa0")
    endif()
    # Check for rpmbuild (Red Hat/Fedora)
    find_program(RPMBUILD_PROGRAM rpmbuild)
    if(RPMBUILD_PROGRAM)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

include(CPack)
