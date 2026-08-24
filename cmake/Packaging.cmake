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
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdl3-0, libx11-6, libfreetype6")
    endif()
    # Check for rpmbuild (Red Hat/Fedora)
    find_program(RPMBUILD_PROGRAM rpmbuild)
    if(RPMBUILD_PROGRAM)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

include(CPack)
