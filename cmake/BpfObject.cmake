# The helper's eBPF UDP byte counter (#3366): src/Platform/Linux/bpf/udp_bytes.bpf.c compiled with clang's BPF
# target, then embedded as a byte array in generated/UdpBytesBpfObject.h, which UdpByteCounter.cpp opens with
# bpf_object__open_mem. Nothing extra is installed, and no bpftool is needed. The program reads no kernel
# structure, so it builds against libbpf's headers and the uapi ones, with no vmlinux.h (maintainer, 2026-11-07).
if(NOT (UNIX AND NOT APPLE))
    return()
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(TASKSMACK_BPF_CLANG "${CMAKE_C_COMPILER}")
else()
    find_program(TASKSMACK_BPF_CLANG NAMES clang-23 clang-22 clang REQUIRED)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(TASKSMACK_BPF_TARGET_ARCH x86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(TASKSMACK_BPF_TARGET_ARCH arm64)
else()
    message(FATAL_ERROR "The eBPF UDP byte counter (#3366) has no BPF target arch for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# The uapi headers (asm/types.h, asm/ptrace.h) live under the multiarch directory on Debian and Ubuntu; the BPF
# target doesn't search it on its own.
set(TASKSMACK_BPF_INCLUDES ${LIBBPF_INCLUDE_DIRS})
if(CMAKE_LIBRARY_ARCHITECTURE)
    list(APPEND TASKSMACK_BPF_INCLUDES /usr/include/${CMAKE_LIBRARY_ARCHITECTURE})
endif()
list(TRANSFORM TASKSMACK_BPF_INCLUDES PREPEND "-I")

set(TASKSMACK_BPF_SOURCE ${CMAKE_SOURCE_DIR}/src/Platform/Linux/bpf/udp_bytes.bpf.c)
set(TASKSMACK_BPF_OBJECT ${CMAKE_BINARY_DIR}/bpf/udp_bytes.bpf.o)
set(TASKSMACK_BPF_HEADER ${CMAKE_BINARY_DIR}/generated/UdpBytesBpfObject.h)

add_custom_command(
    OUTPUT ${TASKSMACK_BPF_OBJECT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bpf
    COMMAND ${TASKSMACK_BPF_CLANG} -target bpf -D__TARGET_ARCH_${TASKSMACK_BPF_TARGET_ARCH} -O2 -g -Wall -Werror
            ${TASKSMACK_BPF_INCLUDES} -c ${TASKSMACK_BPF_SOURCE} -o ${TASKSMACK_BPF_OBJECT}
    DEPENDS ${TASKSMACK_BPF_SOURCE}
    COMMENT "Compiling the eBPF UDP byte counter (#3366)"
    VERBATIM
)
add_custom_command(
    OUTPUT ${TASKSMACK_BPF_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${TASKSMACK_BPF_OBJECT} -DOUTPUT=${TASKSMACK_BPF_HEADER}
            -DNAME=UDP_BYTES_BPF_OBJECT -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS ${TASKSMACK_BPF_OBJECT} ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    COMMENT "Embedding the eBPF UDP byte counter (#3366)"
    VERBATIM
)
add_custom_target(tasksmack_bpf_object DEPENDS ${TASKSMACK_BPF_HEADER})
