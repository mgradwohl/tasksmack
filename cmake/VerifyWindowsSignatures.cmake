# The Authenticode gate on TaskSmackHelper.exe (#2550; #2302). The maintainer's rule: the Windows helper
# ships only Authenticode-signed. Its UAC prompt names its publisher, and only a signature makes that
# prompt say "Verified publisher". So before CPack packages the helper, this refuses unless TaskSmack.exe
# and TaskSmackHelper.exe both carry a signature that is:
#   - Valid (a chain the machine trusts, and the file unchanged since it was signed);
#   - from the expected publisher: the signer certificate's common name is TASKSMACK_EXPECTED_SIGNER,
#     "Matt Gradwohl" by default (the Azure Artifact Signing certificate profile's subject, #2302;
#     docs/code-signing-policy.md), so a test or internal certificate the build machine happens to
#     trust doesn't pass;
#   - timestamped, so it stays valid after the certificate expires;
#   - from the same subject for both.
# Artifact Signing certificates last about three days and rotate, so the publisher is matched by name,
# never by a thumbprint or serial number: the two files may carry different certificates if a rotation
# falls between them, as long as both name the same subject.
# The decision is made in PowerShell, which prints OK or the reason. CMake needs both files' status
# lines and that OK, so output it can't read is a refusal too.
#
# CPack runs it as a CPACK_PRE_BUILD_SCRIPTS script (cmake/CPackProjectConfig.cmake.in), with the staged
# files in CPACK_TEMPORARY_INSTALL_DIRECTORY, only when TASKSMACK_PACKAGE_WINDOWS_HELPER is on. An
# unsigned dev build leaves that off, so this never runs for it. A pipeline that signs the packaged .zip
# instead (TASKSMACK_SIGN_AFTER_PACKAGING; release.yml's sign-windows job) runs it on the signed .zip's
# extracted contents before publishing, as the tests run it on a folder:
#   cmake -DTASKSMACK_VERIFY_DIR=<folder> [-DTASKSMACK_EXPECTED_SIGNER=<CN>] -P cmake/VerifyWindowsSignatures.cmake

if(DEFINED CPACK_TEMPORARY_INSTALL_DIRECTORY)
    set(_ts_dir "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
elseif(DEFINED TASKSMACK_VERIFY_DIR)
    set(_ts_dir "${TASKSMACK_VERIFY_DIR}")
else()
    message(FATAL_ERROR "VerifyWindowsSignatures: no folder to check (CPACK_TEMPORARY_INSTALL_DIRECTORY or TASKSMACK_VERIFY_DIR)")
endif()
if(NOT DEFINED TASKSMACK_EXPECTED_SIGNER OR TASKSMACK_EXPECTED_SIGNER STREQUAL "")
    set(TASKSMACK_EXPECTED_SIGNER "Matt Gradwohl")
endif()

# PowerShell's single-quoted string: a quote is doubled.
function(_ts_quote text out)
    string(REPLACE "'" "''" _quoted "${text}")
    set(${out} "'${_quoted}'" PARENT_SCOPE)
endfunction()

set(_ts_paths "")
foreach(_ts_name IN ITEMS TaskSmack.exe TaskSmackHelper.exe)
    file(GLOB_RECURSE _ts_found LIST_DIRECTORIES false "${_ts_dir}/${_ts_name}")
    list(LENGTH _ts_found _ts_count)
    if(NOT _ts_count EQUAL 1)
        message(FATAL_ERROR "VerifyWindowsSignatures: expected one ${_ts_name} under ${_ts_dir}, found ${_ts_count}")
    endif()
    file(TO_NATIVE_PATH "${_ts_found}" _ts_native)
    _ts_quote("${_ts_native}" _ts_quoted)
    list(APPEND _ts_paths "${_ts_quoted}")
endforeach()
list(JOIN _ts_paths "," _ts_list)
_ts_quote("${TASKSMACK_EXPECTED_SIGNER}" _ts_expected)

find_program(_ts_powershell NAMES pwsh powershell REQUIRED)
# Prints "STATUS <name> <status> <signer CN> <timestamped>" per file, then "OK" or "FAIL <reason>".
string(CONCAT _ts_script
    "$ErrorActionPreference='Stop'; $expected = ${_ts_expected}; $why = $null; "
    "$s = @(${_ts_list}) | ForEach-Object { Get-AuthenticodeSignature -LiteralPath $_ }; "
    "foreach ($x in $s) { "
    "  $name = Split-Path -Leaf $x.Path; "
    "  $cn = if ($x.SignerCertificate) { $x.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) } else { '-' }; "
    "  $stamped = [bool]$x.TimeStamperCertificate; "
    "  Write-Output ('STATUS ' + $name + ' ' + $x.Status + ' ' + $stamped + ' ' + $cn); "
    "  if (-not $why) { "
    "    if ($x.Status -ne 'Valid') { $why = $name + \" isn't Authenticode-signed (\" + $x.Status + ')' } "
    "    elseif ($cn -ne $expected) { $why = $name + ' is signed by ' + $cn + ', not ' + $expected } "
    "    elseif (-not $stamped) { $why = $name + \"'s signature has no timestamp\" } } }; "
    "if (-not $why -and $s[0].SignerCertificate.Subject -ne $s[1].SignerCertificate.Subject) { "
    "  $why = \"TaskSmack.exe and TaskSmackHelper.exe aren't signed by the same subject (\" + "
    "$s[0].SignerCertificate.Subject + ' and ' + $s[1].SignerCertificate.Subject + ')' }; "
    "if ($why) { Write-Output ('FAIL ' + $why) } else { Write-Output 'OK' }")
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
set(_ts_statuses 0)
set(_ts_verdict "")
foreach(_ts_line IN LISTS _ts_lines)
    if(_ts_line MATCHES "^STATUS TaskSmack(Helper)?\\.exe ")
        math(EXPR _ts_statuses "${_ts_statuses} + 1")
    elseif(_ts_line MATCHES "^(OK|FAIL .*)$")
        set(_ts_verdict "${_ts_line}")
    endif()
endforeach()
if(_ts_verdict MATCHES "^FAIL (.*)$")
    message(FATAL_ERROR "VerifyWindowsSignatures: ${CMAKE_MATCH_1}. TaskSmackHelper.exe ships only signed by "
                        "${TASKSMACK_EXPECTED_SIGNER} (#2302): sign TaskSmack.exe and TaskSmackHelper.exe before "
                        "packaging, or configure with -DTASKSMACK_PACKAGE_WINDOWS_HELPER=OFF.\n${_ts_output}")
endif()
if(NOT _ts_statuses EQUAL 2 OR NOT _ts_verdict STREQUAL "OK")
    message(FATAL_ERROR "VerifyWindowsSignatures: couldn't read both signatures:\n${_ts_output}${_ts_error}")
endif()
message(STATUS "VerifyWindowsSignatures: TaskSmack.exe and TaskSmackHelper.exe are signed and timestamped by ${TASKSMACK_EXPECTED_SIGNER}")
