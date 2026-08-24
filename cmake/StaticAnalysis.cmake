# Static analysis and formatting tooling: clang-tidy / clang-format discovery,
# the stripped clang-tidy compilation database, and the run-clang-tidy /
# run-clang-format / copy-compile-commands convenience targets.
#
# Include AFTER TASKSMACK_SOURCES and TASKSMACK_HEADERS are fully defined but
# BEFORE platform-generated sources (e.g., the Windows .rc file) are appended,
# so the analysis targets only see real C++ sources.

if(TASKSMACK_COPY_COMPILE_COMMANDS AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    add_custom_target(copy-compile-commands ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/compile_commands.json"
        BYPRODUCTS "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COMMENT "Copying compile_commands.json to source tree for clangd"
    )
endif()

find_program(CLANG_TIDY_EXE NAMES clang-tidy
    HINTS
        "/usr/lib/llvm-${TASKSMACK_LLVM_VERSION}/bin"
        "$ENV{LLVM_ROOT}/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
        "C:/Program Files/LLVM/bin"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format
    HINTS
        "/usr/lib/llvm-${TASKSMACK_LLVM_VERSION}/bin"
        "$ENV{LLVM_ROOT}/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
        "C:/Program Files/LLVM/bin"
)

if(CLANG_TIDY_EXE)
    # Generate a clean compile_commands.json for clang-tidy in a dedicated directory, stripping flags
    # that clang-tidy cannot handle (C++20 module @*.modmap dependency files, PCH flags).
    # Writing to a *separate* directory preserves the live compile_commands.json that clangd watches;
    # clang-tidy tools point -p at the dedicated directory so clangd is never disturbed.
    set(TASKSMACK_CLANG_TIDY_COMPDB_DIR "${CMAKE_BINARY_DIR}/clang-tidy-compdb")
    if(WIN32)
        add_custom_target(generate-clang-tidy-compile-commands
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}
            COMMAND powershell -NoProfile -Command
                "(Get-Content '${CMAKE_BINARY_DIR}/compile_commands.json' -Raw) -replace '@[^ ]*\\.modmap', '' -replace '-fmodule-output=[^ ]*', '' -replace '-Xclang -include-pch -Xclang [^ ]*', '' -replace '-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*', '' -replace '-Xclang -fno-pch-timestamp', '' | Set-Content '${TASKSMACK_CLANG_TIDY_COMPDB_DIR}/compile_commands.json' -NoNewline"
            DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
            COMMENT "Writing clang-tidy compile_commands.json (module/PCH flags stripped)"
        )
    else()
        add_custom_target(generate-clang-tidy-compile-commands
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}/compile_commands.json
            COMMAND sed -i
                -e "s/@[^ ]*\\.modmap//g"
                -e "s/-fmodule-output=[^ ]*//g"
                -e "s/-Xclang -include-pch -Xclang [^ ]*//g"
                -e "s/-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*//g"
                -e "s/-Xclang -fno-pch-timestamp//g"
                ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}/compile_commands.json
            DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
            COMMENT "Writing clang-tidy compile_commands.json (module/PCH flags stripped)"
        )
    endif()

    add_custom_target(run-clang-tidy
        COMMAND ${CLANG_TIDY_EXE}
            --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
            -p ${TASKSMACK_CLANG_TIDY_COMPDB_DIR}
            --extra-arg=-std=c++23
            --extra-arg=-Wno-unknown-warning-option
            ${TASKSMACK_SOURCES}
            ${TASKSMACK_HEADERS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy on project sources"
    )
    # Dependencies:
    # - generate-clang-tidy-compile-commands: strips C++20 module flags from compile_commands.json
    # - glad_gl_core_33: ensures GLAD headers are generated before clang-tidy runs
    # - copy-compile-commands (optional): keeps the source-root copy fresh when enabled
    add_dependencies(run-clang-tidy generate-clang-tidy-compile-commands glad_gl_core_33)
    if(TARGET copy-compile-commands)
        add_dependencies(run-clang-tidy copy-compile-commands)
    endif()
endif()

if(CLANG_FORMAT_EXE)
    add_custom_target(run-clang-format
        COMMAND ${CLANG_FORMAT_EXE} -i
            ${TASKSMACK_SOURCES}
            ${TASKSMACK_HEADERS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Applying clang-format to project sources"
    )
endif()
