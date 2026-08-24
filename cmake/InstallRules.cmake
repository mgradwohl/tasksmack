# Install rules for the TaskSmack binary, third-party static libs, runtime assets,
# and the FreeType runtime copy for portable layouts.
# Include AFTER the TaskSmack target is defined and PROJECT_NAME_LOWER is set.

include(GNUInstallDirs)

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
            RUNTIME DESTINATION .
            LIBRARY DESTINATION .
        )
    endif()
endif()

# Note: SDL3 handles X11/Wayland internally, no explicit X11 linking needed

install(TARGETS TaskSmack
    BUNDLE DESTINATION .
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
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
