# Pre-detect an RC compiler on Windows and set CMAKE_RC_COMPILER before project() declaration.
# NOTE: Included from CMakeLists.txt BEFORE calling project(), so CMake's platform detection
# doesn't fail when looking for the RC compiler. Must stay a plain include (not a function/macro)
# so the CMAKE_RC_COMPILER it sets is visible to the including scope at that point.
#
# This project builds with the plain Clang driver (not clang-cl), which is a toolchain pairing
# CMake bridges to MSVC's rc.exe via an internal two-stage helper (`cmake -E cmake_llvm_rc`:
# Clang preprocesses the .rc file, then rc.exe compiles the already-preprocessed output). That
# bridge is known to hang indefinitely for this project (issue #746: reproduced repeatedly, both
# in CI and locally, always with a still-running rc.exe process using 0% CPU and no output) rather
# than failing fast, with no reproducible root cause in rc.exe itself -- running rc.exe standalone
# on the exact .pp file CMake generates fails immediately instead of hanging, so the hang is
# specific to CMake's bridging of the two processes, not to rc.exe or a missing windows.h/INCLUDE
# setup as originally suspected.
#
# CMake still runs llvm-rc through that same two-stage `cmake_llvm_rc` bridge -- that part is
# inherent to pairing the plain Clang driver with any rc-family compiler, not specific to MSVC's
# rc.exe -- but empirically the bridge does not hang with llvm-rc as its second stage (verified
# with full clean configure+build+test runs). llvm-rc also resolves headers like <windows.h> via
# Clang's own MSVC/SDK auto-detection -- the same mechanism Clang already uses for ordinary C++
# compiles -- which sidesteps the INCLUDE-environment-variable conflict from issue #686 (SDK
# rc.exe needs INCLUDE set for windows.h, but setting INCLUDE breaks Clang's own MSVC STL
# auto-detection): llvm-rc needs neither an INCLUDE-based nor a Developer Command Prompt
# environment, so this works unmodified outside a VS developer shell too.
#
# No automatic fallback to the Windows SDK's rc.exe: LLVM (which includes llvm-rc) is already a
# required prerequisite (see CONTRIBUTING.md), so a missing llvm-rc means a broken/incomplete dev
# environment -- fail fast with an actionable error instead of silently falling back to the exact
# tool known to trigger the #746 hang. Users who intentionally need rc.exe (or any other RC
# compiler) can still opt in explicitly via -DCMAKE_RC_COMPILER=... or the RC environment
# variable, below.
#
# Skip auto-detection when the caller pinned an RC compiler via -DCMAKE_RC_COMPILER=... or the
# standard RC environment variable (CMake's own convention, analogous to CC/CXX) -- auto-detection
# must not silently override an explicit toolchain choice. But CMAKE_RC_COMPILER also ends up
# cached (by CMake's own project()/enable_language(RC) machinery) after any successful configure,
# including a plain default-detected value from before this pre-detection block existed at all --
# so "CMAKE_RC_COMPILER is cached" alone can't distinguish a genuine override from a stale cache
# entry left over from an old build tree, and CMake's normal set(... CACHE ...) semantics (which
# never overwrite an existing entry without FORCE) would otherwise leave such a tree stuck on the
# exact rc.exe implicated in the #746 hang forever.
#
# TASKSMACK_AUTO_RC_COMPILER records our own last auto-selection so a cached value that still
# matches it is recognized as "ours", not a user override, and safely re-detected/refreshed. A
# cached value that DOESN'T match it is normally trusted as a deliberate override -- except
# specifically when it's the SDK's rc.exe by filename, which is untrusted and evicted instead:
# that's the one tool with a confirmed hang, and on a tree from before this fix landed there's no
# marker yet to tell a stale default-detected rc.exe apart from someone deliberately choosing it
# via -DCMAKE_RC_COMPILER on that same invocation, so this narrowly favors safety over that rare,
# ambiguous case. A build tree configured before this fix therefore self-heals on its next
# reconfigure without needing to be deleted. Anyone who deliberately wants rc.exe specifically
# should set the RC environment variable instead (checked first, below, and always honored)
# rather than relying on -DCMAKE_RC_COMPILER alone.
if(WIN32)
    if(DEFINED CACHE{CMAKE_RC_COMPILER} AND NOT DEFINED CACHE{TASKSMACK_AUTO_RC_COMPILER})
        get_filename_component(_TASKSMACK_RC_CACHED_NAME "$CACHE{CMAKE_RC_COMPILER}" NAME)
        if(_TASKSMACK_RC_CACHED_NAME MATCHES "^[Rr][Cc]\\.[Ee][Xx][Ee]$")
            unset(CMAKE_RC_COMPILER CACHE)
        endif()
    endif()

    set(_TASKSMACK_RC_IS_OVERRIDE FALSE)
    if(DEFINED CACHE{CMAKE_RC_COMPILER} AND
       (NOT DEFINED CACHE{TASKSMACK_AUTO_RC_COMPILER} OR
        NOT "$CACHE{CMAKE_RC_COMPILER}" STREQUAL "${TASKSMACK_AUTO_RC_COMPILER}"))
        set(_TASKSMACK_RC_IS_OVERRIDE TRUE)
    endif()

    if(DEFINED ENV{RC})
        # Set CMAKE_RC_COMPILER from RC ourselves, via CACHE ... FORCE, rather than relying on
        # CMake's own project()/enable_language(RC) to pick up the environment variable: on an
        # existing build tree, CMake reuses its previously generated per-language compiler-info
        # file (CMakeFiles/<ver>/CMakeRCCompiler.cmake) without re-checking RC at all, merely
        # unsetting the CMAKE_RC_COMPILER cache entry doesn't invalidate that file, and the stale
        # compiler stays silently active -- confirmed by reproducing exactly that (RC ignored,
        # prior compiler still selected) with an unset()-only version of this branch.
        set(CMAKE_RC_COMPILER "$ENV{RC}" CACHE FILEPATH "RC compiler" FORCE)
        unset(TASKSMACK_AUTO_RC_COMPILER CACHE)
        message(STATUS "Using user-specified RC compiler (RC environment variable): $ENV{RC}")
    elseif(_TASKSMACK_RC_IS_OVERRIDE)
        message(STATUS "Using user-specified RC compiler (CMAKE_RC_COMPILER): $CACHE{CMAKE_RC_COMPILER}")
    else()
        # NO_CACHE (CMake 3.21+) so this genuinely re-searches every configure instead of
        # freezing onto whatever it found the first time -- a cached result would keep forcibly
        # restoring a stale llvm-rc.exe path via the CACHE FORCE below even after LLVM_ROOT
        # changes or that install is removed. Also evict any LLVM_RC_FOUND cache entry left over
        # from earlier versions of this file (before NO_CACHE was added), so it doesn't linger
        # unused in CMakeCache.txt.
        unset(LLVM_RC_FOUND CACHE)
        # HINTS so LLVM_ROOT (what the Windows presets/CI use to locate Clang -- see
        # CMakePresets.json / .github/actions/setup-windows-llvm) is checked before PATH,
        # matching the LLVM-tool discovery in cmake/StaticAnalysis.cmake. Without this,
        # llvm-rc.exe sitting right next to the selected clang.exe could be missed if LLVM's bin
        # directory isn't separately on PATH.
        find_program(LLVM_RC_FOUND llvm-rc
            HINTS
                "$ENV{LLVM_ROOT}/bin"
                "$ENV{ProgramFiles}/LLVM/bin"
                "C:/Program Files/LLVM/bin"
            NO_CACHE
        )
        if(LLVM_RC_FOUND)
            # CACHE ... FORCE, not a plain set(): CMake's own project()/enable_language(RC)
            # machinery consults the CACHE entry directly to decide whether a compiler is already
            # "determined" for this build tree, so a plain-scope set() here would not actually
            # evict a stale CACHE{CMAKE_RC_COMPILER} value (e.g. rc.exe left over from before this
            # fix existed) -- confirmed by reproducing the #746 hang again on a simulated stale
            # tree until this FORCE was added.
            set(CMAKE_RC_COMPILER ${LLVM_RC_FOUND} CACHE FILEPATH "RC compiler" FORCE)
            set(TASKSMACK_AUTO_RC_COMPILER ${LLVM_RC_FOUND} CACHE INTERNAL
                "Last RC compiler path auto-selected by cmake/RCCompiler.cmake; used to detect stale cache entries.")
            message(STATUS "Found RC compiler: ${CMAKE_RC_COMPILER}")
        else()
            # No RC compiler found anywhere. CONTRIBUTING.md documents LLVM (which includes
            # llvm-rc) as a required prerequisite, installed automatically by tools/setup-dev.ps1
            # -- so this only happens on a broken/incomplete dev environment, never on a
            # legitimately RC-less platform. Fail loudly instead of silently falling back to a
            # tool known to hang (see above) or shipping an .exe missing its icon and version info
            # (the DPI-awareness manifest is embedded independently via /MANIFESTINPUT at link
            # time -- see the "Windows resource file" section in CMakeLists.txt -- so it's
            # unaffected by RC availability); this matches the FATAL_ERROR checks there for the
            # .rc.in/manifest templates, which are equally "required for Windows builds."
            message(FATAL_ERROR "llvm-rc not found (searched LLVM_ROOT/bin, standard LLVM install "
                                 "locations, and PATH). Install LLVM (see CONTRIBUTING.md's Windows "
                                 "Pre-Requisites), e.g. by running tools/setup-dev.ps1. To use a "
                                 "different RC compiler instead, pass -DCMAKE_RC_COMPILER=<path> or "
                                 "set the RC environment variable.")
        endif()
    endif()
endif()
