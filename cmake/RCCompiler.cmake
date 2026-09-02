# Pre-detect RC.exe on Windows and set CMAKE_RC_COMPILER before project() declaration.
# If RC.exe is not found, search for alternatives (llvm-rc, llvm-windres).
# NOTE: Included from CMakeLists.txt BEFORE calling project(), so CMake's platform detection
# doesn't fail when looking for the RC compiler. Must stay a plain include (not a function/macro)
# so the CMAKE_RC_COMPILER it sets is visible to the including scope at that point.
if(WIN32 AND NOT CMAKE_RC_COMPILER AND "$ENV{RC}" STREQUAL "")
    # Our Windows CI installs only LLVM/Ninja/ccache (no Visual Studio Developer Command
    # Prompt), so rc.exe is never on PATH; it has to be located inside the Windows SDK
    # install instead. The SDK's bin directory is versioned (e.g. 10.0.26100.0) and that
    # version changes whenever the SDK is updated -- including unannounced bumps to the
    # windows-2025 GitHub Actions runner image -- so glob for whatever version is actually
    # installed rather than hardcoding one, and prefer the highest version found.
    # %ProgramFiles(x86)%/%ProgramFiles% come from the environment with backslash
    # separators; normalize to forward slashes before building glob patterns from them,
    # otherwise the matched path can mix backslashes with the pattern's forward slashes,
    # and CMake later chokes trying to re-parse that raw backslash out of its own
    # generated CMakeRCCompiler.cmake (e.g. "\P" in "Program Files" as an invalid escape).
    # Plain string(REPLACE) rather than file(TO_CMAKE_PATH): the latter treats its input
    # as a native PATH-style list and its behavior differs by host OS, which isn't what a
    # single directory path needs here.
    string(REPLACE "\\" "/" TASKSMACK_PROGRAMFILES_X86 "$ENV{ProgramFiles\(x86\)}")
    string(REPLACE "\\" "/" TASKSMACK_PROGRAMFILES "$ENV{ProgramFiles}")
    file(GLOB WINSDK_RC_CANDIDATES
        "${TASKSMACK_PROGRAMFILES_X86}/Windows Kits/10/bin/*/x64/rc.exe"
        "${TASKSMACK_PROGRAMFILES_X86}/Windows Kits/11/bin/*/x64/rc.exe"
        "${TASKSMACK_PROGRAMFILES}/Windows Kits/10/bin/*/x64/rc.exe"
        "${TASKSMACK_PROGRAMFILES}/Windows Kits/11/bin/*/x64/rc.exe"
    )
    if(WINSDK_RC_CANDIDATES)
        list(SORT WINSDK_RC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
        list(GET WINSDK_RC_CANDIDATES 0 RC_COMPILER_FOUND)
    endif()

    if(NOT RC_COMPILER_FOUND)
        # Fall back to PATH (Developer Command Prompt, custom installs, etc.).
        find_program(RC_COMPILER_FOUND rc.exe)
    endif()

    if(RC_COMPILER_FOUND)
        set(CMAKE_RC_COMPILER ${RC_COMPILER_FOUND})
        message(STATUS "Found RC compiler: ${CMAKE_RC_COMPILER}")
    else()
        # RC.exe not found; try alternatives: llvm-rc, llvm-windres
        find_program(LLVM_RC_FOUND llvm-rc)
        if(LLVM_RC_FOUND)
            set(CMAKE_RC_COMPILER ${LLVM_RC_FOUND})
            message(STATUS "Found RC compiler alternative: ${CMAKE_RC_COMPILER}")
        else()
            find_program(LLVM_WINDRES_FOUND llvm-windres)
            if(LLVM_WINDRES_FOUND)
                set(CMAKE_RC_COMPILER ${LLVM_WINDRES_FOUND})
                message(STATUS "Found RC compiler alternative: ${CMAKE_RC_COMPILER}")
            else()
                # No RC compiler found anywhere. CONTRIBUTING.md documents the Windows SDK
                # (which includes rc.exe) as a required prerequisite, and setup-dev.ps1
                # installs it automatically -- so this only happens on a broken/incomplete
                # dev environment, never on a legitimately RC-less platform. Fail loudly
                # instead of silently shipping an .exe missing its icon, version info, and
                # DPI-awareness manifest; this matches the FATAL_ERROR checks below for the
                # .rc.in/manifest templates, which are equally "required for Windows builds."
                message(FATAL_ERROR "No RC compiler found (tried Windows SDK, PATH, llvm-rc, llvm-windres). "
                                     "Install the Windows SDK (see CONTRIBUTING.md's Windows Pre-Requisites), "
                                     "e.g. by running tools/setup-dev.ps1.")
            endif()
        endif()
    endif()
endif()
