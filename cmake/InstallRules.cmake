# Install rules for the TaskSmack binary, third-party static libs, runtime assets,
# and the FreeType runtime copy for portable layouts.
# Include AFTER the TaskSmack target is defined and PROJECT_NAME_LOWER is set.

include(GNUInstallDirs)

# Linux packages (.deb/.tar.gz) install the executable under bin/ so the .deb lands it in
# /usr/bin (on PATH, conventional FHS layout -- see #845) and the .tar.gz gets a bin/TaskSmack
# layout; src/UI/AssetPath.cpp's selectAssetsDir() already has a "one level up" candidate
# (exeDir/../share/<app>/assets) specifically for this case. Windows keeps the existing flat "."
# layout (TaskSmack.exe at the .zip root) unchanged, since that layout is documented and NSIS
# packaging is untested from this change.
if(WIN32)
    set(TASKSMACK_RUNTIME_DESTINATION .)
else()
    set(TASKSMACK_RUNTIME_DESTINATION bin)
endif()

# Copy FreeType runtime (if shared/imported) next to the executable for portable layout
if(TARGET Freetype::Freetype)
    get_target_property(_ft_alias Freetype::Freetype ALIASED_TARGET)
    if(_ft_alias)
        set(_ft_target ${_ft_alias})
    else()
        set(_ft_target Freetype::Freetype)
    endif()

    get_target_property(_ft_imported ${_ft_target} IMPORTED)
    if(_ft_imported)
        set(_ft_runtime_file $<TARGET_FILE:${_ft_target}>)
        set(_ft_needs_copy ON)
    else()
        get_target_property(_ft_type ${_ft_target} TYPE)
        if(_ft_type STREQUAL "SHARED_LIBRARY" OR _ft_type STREQUAL "MODULE_LIBRARY")
            set(_ft_runtime_file $<TARGET_FILE:${_ft_target}>)
            set(_ft_needs_copy ON)
        endif()
    endif()

    if(_ft_needs_copy)
        add_custom_command(TARGET TaskSmack POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_ft_runtime_file}
                $<TARGET_FILE_DIR:TaskSmack>
            COMMENT "Copying FreeType runtime to build directory")

        install(TARGETS ${_ft_target}
            RUNTIME DESTINATION ${TASKSMACK_RUNTIME_DESTINATION}
            LIBRARY DESTINATION ${TASKSMACK_RUNTIME_DESTINATION}
        )
    endif()
endif()

# Note: SDL3 handles X11/Wayland internally, no explicit X11 linking needed

install(TARGETS TaskSmack
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${TASKSMACK_RUNTIME_DESTINATION}
    LIBRARY DESTINATION ${TASKSMACK_RUNTIME_DESTINATION}
    ARCHIVE DESTINATION .
)

# Install static libs alongside the binary for local/portable layout
install(TARGETS imgui_lib implot_lib glad_gl_core_33
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    ARCHIVE DESTINATION .
)

# Install runtime assets (fonts/themes/icons) using GNUInstallDirs for FHS compliance.
# On Linux this resolves to share/<project_name_lower>/assets/; on Windows the prefix root is typically
# the install directory so share/<project_name_lower>/assets/ lives alongside the binary.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/fonts
        ${CMAKE_SOURCE_DIR}/assets/themes
        ${CMAKE_SOURCE_DIR}/assets/icons
    DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME_LOWER}/assets
)

# Desktop integration (Linux only, see #845): a system install (the .deb) is now on PATH
# (Exec=TaskSmack, matching TASKSMACK_RUNTIME_DESTINATION=bin above), so also install the
# existing launcher entry and hicolor icons that tools/install-local.sh already installs for a
# local dev install, so a packaged install is equally discoverable from an application menu.
# Included for both TGZ and DEB since both share these install() rules -- harmless extra files
# in the portable .tar.gz, which is still just "extract and run bin/TaskSmack" for anyone who
# doesn't want a system install. Icon/menu caches (update-desktop-database,
# gtk-update-icon-cache) are not triggered automatically by this CPack-generated .deb (no
# postinst hook), so the entry may need a desktop-session restart to appear, same as it would
# for any manually-copied .desktop file.
if(NOT WIN32)
    install(FILES ${CMAKE_SOURCE_DIR}/assets/linux/app.tasksmack.TaskSmack.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    foreach(_ts_icon_size 16 24 32 48 128 256)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/tasksmack-${_ts_icon_size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_ts_icon_size}x${_ts_icon_size}/apps
            RENAME app.tasksmack.TaskSmack.png
        )
    endforeach()
    install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/tasksmack.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME app.tasksmack.TaskSmack.svg
    )
endif()
