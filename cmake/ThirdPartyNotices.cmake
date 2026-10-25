# THIRD_PARTY_NOTICES.txt (#2490): every dependency compiled into TaskSmack, at the version it is pinned
# to, with its licence and upstream, and what TaskSmack changes in it. Generated from the pins in
# Dependencies.cmake, so a Renovate bump can't leave it stale; installed beside the licence files by
# InstallRules.cmake.
#
# ts_write_third_party_notices(<output file>) writes it. Each dependency's licence identifier and
# notes are listed here; a FetchContent_Declare() in Dependencies.cmake that isn't fails the configure,
# so a new dependency can't ship without a notice.

set(_TS_NOTICE_LICENSES
    "spdlog|MIT"
    "tomlplusplus|MIT"
    "stb|MIT OR Unlicense"
    "SDL3|Zlib"
    "glad|MIT (the generator), and (WTFPL OR CC0-1.0) AND Apache-2.0 (the generated OpenGL loader)"
    "freetype|FTL OR GPL-2.0-or-later (used under the FTL)"
    "imgui|MIT"
    "implot|MIT")

function(ts_write_third_party_notices output)
    file(READ "${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake" _deps)
    string(REGEX MATCHALL
           "FetchContent_Declare\\(\n +[A-Za-z0-9_]+\n +GIT_REPOSITORY [^\n]+\n +GIT_TAG [0-9a-f]+[^\n]*"
           _declares "${_deps}")
    if(NOT _declares)
        message(FATAL_ERROR "ThirdPartyNotices: no pinned FetchContent_Declare() found in Dependencies.cmake")
    endif()

    set(_text "Third-party software in TaskSmack ${PROJECT_VERSION}\n")
    string(APPEND _text "==========================================\n\n")
    string(APPEND _text "TaskSmack compiles in the code below, each pinned to the commit shown. Each one's licence\n")
    string(APPEND _text "text is in the third-party licence folder beside this file.\n")

    foreach(_declare IN LISTS _declares)
        string(REGEX MATCH "FetchContent_Declare\\(\n +([A-Za-z0-9_]+)\n +GIT_REPOSITORY ([^\n]+)\n +GIT_TAG ([0-9a-f]+) *#? *([^\n]*)"
               _ "${_declare}")
        set(_name "${CMAKE_MATCH_1}")
        string(STRIP "${CMAKE_MATCH_2}" _repo)
        set(_commit "${CMAKE_MATCH_3}")
        # The pin's comment reads "<version> - pinned to SHA ..."; one with no version (stb has no
        # releases) says only that. No ';' in the licences below: CMake would split the entry there.
        string(REGEX REPLACE " *- *[Pp]inned.*$" "" _version "${CMAKE_MATCH_4}")
        if(_version MATCHES "^[Pp]inned" OR _version STREQUAL "")
            set(_version "(untagged)")
        endif()

        set(_license "")
        foreach(_entry IN LISTS _TS_NOTICE_LICENSES)
            string(REPLACE "|" ";" _entry "${_entry}")
            list(GET _entry 0 _entry_name)
            if(_entry_name STREQUAL _name)
                list(GET _entry 1 _license)
            endif()
        endforeach()
        if(_license STREQUAL "")
            message(FATAL_ERROR "ThirdPartyNotices: add ${_name}'s licence to _TS_NOTICE_LICENSES in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
        endif()

        string(APPEND _text "\n${_name} ${_version}\n")
        string(APPEND _text "  Upstream: ${_repo}\n")
        string(APPEND _text "  Commit:   ${_commit}\n")
        string(APPEND _text "  Licence:  ${_license}\n")
        if(_name STREQUAL "tomlplusplus")
            # The patch InstallRules.cmake installs, by the name Dependencies.cmake gives it (#2779). Run
            # standalone (the -P check, #2753) nothing has included Dependencies.cmake, so the one
            # definition, its set(TASKSMACK_TOMLPLUSPLUS_PATCH "${CMAKE_CURRENT_LIST_DIR}/<file>"), is read
            # from the file instead of being declared a second time here.
            if(NOT DEFINED TASKSMACK_TOMLPLUSPLUS_PATCH)
                string(REGEX MATCH "\nset\\(TASKSMACK_TOMLPLUSPLUS_PATCH \"\\$\\{CMAKE_CURRENT_LIST_DIR\\}/([^\"]+)\"\\)"
                       _ "${_deps}")
                if(NOT CMAKE_MATCH_1)
                    message(FATAL_ERROR "ThirdPartyNotices: ${_deps_file} doesn't set TASKSMACK_TOMLPLUSPLUS_PATCH as "
                                        "\"\${CMAKE_CURRENT_LIST_DIR}/<file>\" (#2779), so the notice can't name the patch.")
                endif()
                cmake_path(GET _deps_file PARENT_PATH _deps_dir)
                set(TASKSMACK_TOMLPLUSPLUS_PATCH "${_deps_dir}/${CMAKE_MATCH_1}")
            endif()
            cmake_path(GET TASKSMACK_TOMLPLUSPLUS_PATCH FILENAME _patch_name)
            string(APPEND _text "  Modified: patched with ${_patch_name} (installed beside this file),\n")
            string(APPEND _text "            so malformed TOML is reported as a parse error instead of reaching\n")
            string(APPEND _text "            the parser's assertions (undefined behaviour in release builds).\n")
        endif()
    endforeach()

    file(WRITE "${output}" "${_text}")
endfunction()
