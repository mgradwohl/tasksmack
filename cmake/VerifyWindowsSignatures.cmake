# The Authenticode gate on TaskSmackHelper.exe (#2550; #2302). The maintainer's rule: the Windows helper
# ships only Authenticode-signed. Its UAC prompt names its publisher, and only a signature makes that
# prompt say "Verified publisher". So before CPack packages the helper, this refuses unless both it and
# TaskSmack.exe carry a valid Authenticode signature from one signer.
#
# CPack runs it as a CPACK_PRE_BUILD_SCRIPTS script (cmake/CPackProjectConfig.cmake.in), with the staged
# files in CPACK_TEMPORARY_INSTALL_DIRECTORY, only when TASKSMACK_PACKAGE_WINDOWS_HELPER is on. An
# unsigned dev or dry-run build leaves that off, so this never runs for it. The tests run it on a folder:
#   cmake -DTASKSMACK_VERIFY_DIR=<folder> -P cmake/VerifyWindowsSignatures.cmake

if(DEFINED CPACK_TEMPORARY_INSTALL_DIRECTORY)
    set(_ts_dir "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
elseif(DEFINED TASKSMACK_VERIFY_DIR)
    set(_ts_dir "${TASKSMACK_VERIFY_DIR}")
else()
    message(FATAL_ERROR "VerifyWindowsSignatures: no folder to check (CPACK_TEMPORARY_INSTALL_DIRECTORY or TASKSMACK_VERIFY_DIR)")
endif()

set(_ts_paths "")
foreach(_ts_name IN ITEMS TaskSmack.exe TaskSmackHelper.exe)
    file(GLOB_RECURSE _ts_found LIST_DIRECTORIES false "${_ts_dir}/${_ts_name}")
    list(LENGTH _ts_found _ts_count)
    if(NOT _ts_count EQUAL 1)
        message(FATAL_ERROR "VerifyWindowsSignatures: expected one ${_ts_name} under ${_ts_dir}, found ${_ts_count}")
    endif()
    file(TO_NATIVE_PATH "${_ts_found}" _ts_native)
    string(REPLACE "'" "''" _ts_quoted "${_ts_native}")
    list(APPEND _ts_paths "'${_ts_quoted}'")
endforeach()
list(JOIN _ts_paths "," _ts_list)

find_program(_ts_powershell NAMES pwsh powershell REQUIRED)
# One line per file, "<name> <status> <signer thumbprint>", then "SAME" or "DIFFERENT" signers.
string(CONCAT _ts_script
    "$ErrorActionPreference='Stop'; $s = @(${_ts_list}) | ForEach-Object { Get-AuthenticodeSignature -LiteralPath $_ }; "
    "foreach ($x in $s) { $t = if ($x.SignerCertificate) { $x.SignerCertificate.Thumbprint } else { '-' }; "
    "Write-Output ((Split-Path -Leaf $x.Path) + ' ' + $x.Status + ' ' + $t) }; "
    "if ($s[0].SignerCertificate -and $s[1].SignerCertificate -and "
    "$s[0].SignerCertificate.Thumbprint -eq $s[1].SignerCertificate.Thumbprint) { 'SAME' } else { 'DIFFERENT' }")
execute_process(
    COMMAND "${_ts_powershell}" -NoProfile -NonInteractive -Command "${_ts_script}"
    OUTPUT_VARIABLE _ts_output
    ERROR_VARIABLE _ts_error
    RESULT_VARIABLE _ts_result
    TIMEOUT 120)
if(NOT _ts_result EQUAL 0)
    message(FATAL_ERROR "VerifyWindowsSignatures: couldn't read the signatures (${_ts_result}): ${_ts_error}")
endif()

string(REPLACE "\r" "" _ts_output "${_ts_output}")
string(REPLACE "\n" ";" _ts_lines "${_ts_output}")
foreach(_ts_line IN LISTS _ts_lines)
    if(_ts_line MATCHES "^(TaskSmack(Helper)?\\.exe) ([A-Za-z]+) ")
        if(NOT CMAKE_MATCH_3 STREQUAL "Valid")
            message(FATAL_ERROR "VerifyWindowsSignatures: ${CMAKE_MATCH_1} isn't Authenticode-signed (${CMAKE_MATCH_3}). "
                                "TaskSmackHelper.exe ships only signed (#2302): sign TaskSmack.exe and TaskSmackHelper.exe "
                                "before packaging, or configure with -DTASKSMACK_PACKAGE_WINDOWS_HELPER=OFF.")
        endif()
    endif()
endforeach()
if(NOT _ts_output MATCHES "\nSAME|^SAME")
    message(FATAL_ERROR "VerifyWindowsSignatures: TaskSmack.exe and TaskSmackHelper.exe aren't signed by the same signer:\n${_ts_output}")
endif()
message(STATUS "VerifyWindowsSignatures: TaskSmack.exe and TaskSmackHelper.exe are Authenticode-signed by one signer")
