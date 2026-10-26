# The Authenticode gate on TaskSmackHelper.exe (#2550; #2302). The maintainer's rule: the Windows helper
# ships only Authenticode-signed. Its UAC prompt names its publisher, and only a signature makes that
# prompt say "Verified publisher". So before CPack packages the helper, this refuses unless TaskSmack.exe
# and TaskSmackHelper.exe both carry a signature that is:
#   - Valid (a chain the machine trusts, and the file unchanged since it was signed);
#   - from the expected publisher: the signer certificate's common name is TASKSMACK_EXPECTED_SIGNER,
#     "Matt Gradwohl" by default (the Azure Artifact Signing certificate profile's subject, #2302;
#     docs/code-signing-policy.md);
#   - from Azure Artifact Signing (#2990): the signer certificate's issuer is like
#     TASKSMACK_EXPECTED_ISSUER, by default one of Microsoft's ID Verified code-signing CAs (today
#     "CN=Microsoft ID Verified CS EOC CA 04, O=Microsoft Corporation, C=US"), and the certificate
#     carries an extended key usage like TASKSMACK_EXPECTED_EKU, by default Artifact Signing's
#     1.3.6.1.4.1.311.97.*. The common name alone isn't enough: a self-signed or internal test
#     certificate with CN=Matt Gradwohl that the build machine trusts has it too;
#   - timestamped, so it stays valid after the certificate expires;
#   - from the same subject for both.
# Artifact Signing certificates last about three days and rotate, so the publisher is matched by name,
# issuer and extended key usage, never by a thumbprint or serial number: the two files may carry
# different certificates if a rotation falls between them, as long as both name the same subject. The
# issuer pattern takes any two-digit CA number, so a rollover from CA 04 to CA 05 still passes.
# The decision is made in PowerShell, which prints OK or the reason. CMake needs both files' status
# lines and that OK, so output it can't read is a refusal too.
#
# CPack runs it as a CPACK_PRE_BUILD_SCRIPTS script (cmake/CPackProjectConfig.cmake.in), with the staged
# files in CPACK_TEMPORARY_INSTALL_DIRECTORY, only when TASKSMACK_PACKAGE_WINDOWS_HELPER is on. An
# unsigned dev build leaves that off, so this never runs for it. A pipeline that signs the packaged .zip
# instead (TASKSMACK_SIGN_AFTER_PACKAGING; release.yml's sign-windows job) runs it on the signed .zip's
# extracted contents before publishing, as the tests run it on a folder:
#   cmake -DTASKSMACK_VERIFY_DIR=<folder> [-DTASKSMACK_EXPECTED_SIGNER=<CN>]
#         [-DTASKSMACK_EXPECTED_ISSUER=<pattern>] [-DTASKSMACK_EXPECTED_EKU=<pattern>]
#         -P cmake/VerifyWindowsSignatures.cmake
# The two patterns are PowerShell -like wildcards (* ? [AE] [0-9]), matched against the whole issuer
# name and against each extended key usage OID. Left empty, each takes its default, so a pin can't be
# dropped by mistake.
#
# The decision is the PowerShell function Test-TaskSmackSignatures below. Its test,
# tools/test-verify-windows-signatures.ps1, has it written to a file, without checking anything:
#   cmake -DTASKSMACK_WRITE_SIGNATURE_CHECK=<file.ps1> -P cmake/VerifyWindowsSignatures.cmake
# and calls it with certificates made in memory, so no certificate store is touched.

# Prints "STATUS <name> <status> <timestamped> <signer CN>" per signature, then "OK" or "FAIL <reason>".
# $Signatures are Get-AuthenticodeSignature's results, or objects with the same Path, Status,
# SignerCertificate (an X509Certificate2) and TimeStamperCertificate.
set(_ts_check [=[
function Test-TaskSmackSignatures {
    param($Signatures, [string]$ExpectedSigner, [string]$ExpectedIssuer, [string]$ExpectedEku)
    $why = $null
    $Signatures = @($Signatures)
    foreach ($x in $Signatures) {
        $name = Split-Path -Leaf $x.Path
        $cert = $x.SignerCertificate
        $cn = '-'
        $issuer = '-'
        $ekus = @()
        if ($cert) {
            $cn = $cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
            $issuer = $cert.Issuer
            $ekus = @($cert.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' } | ForEach-Object {
                    [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($_, $_.Critical).EnhancedKeyUsages
                } | ForEach-Object { $_.Value })
        }
        $stamped = [bool]$x.TimeStamperCertificate
        Write-Output ('STATUS ' + $name + ' ' + $x.Status + ' ' + $stamped + ' ' + $cn)
        if (-not $why) {
            if ([string]$x.Status -ne 'Valid') { $why = $name + " isn't Authenticode-signed (" + $x.Status + ')' }
            elseif ($cn -ne $ExpectedSigner) { $why = $name + ' is signed by ' + $cn + ', not ' + $ExpectedSigner }
            elseif ($issuer -notlike $ExpectedIssuer) {
                $why = $name + "'s signer certificate is issued by " + $issuer + ', not by ' + $ExpectedIssuer
            }
            elseif (-not @($ekus | Where-Object { $_ -like $ExpectedEku }).Count) {
                $has = if ($ekus.Count) { $ekus -join ', ' } else { 'none' }
                $why = $name + "'s signer certificate has no " + $ExpectedEku + ' extended key usage (it has ' + $has + ')'
            }
            elseif (-not $stamped) { $why = $name + "'s signature has no timestamp" }
        }
    }
    if (-not $why -and $Signatures.Count -ne 2) { $why = 'expected two signatures, got ' + $Signatures.Count }
    if (-not $why -and $Signatures[0].SignerCertificate.Subject -ne $Signatures[1].SignerCertificate.Subject) {
        $why = "TaskSmack.exe and TaskSmackHelper.exe aren't signed by the same subject (" +
            $Signatures[0].SignerCertificate.Subject + ' and ' + $Signatures[1].SignerCertificate.Subject + ')'
    }
    if ($why) { Write-Output ('FAIL ' + $why) } else { Write-Output 'OK' }
}
]=])
if(DEFINED TASKSMACK_WRITE_SIGNATURE_CHECK)
    file(WRITE "${TASKSMACK_WRITE_SIGNATURE_CHECK}" "${_ts_check}")
    return()
endif()

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
# The defaults here and in cmake/Options.cmake are the same.
if(NOT DEFINED TASKSMACK_EXPECTED_ISSUER OR TASKSMACK_EXPECTED_ISSUER STREQUAL "")
    set(TASKSMACK_EXPECTED_ISSUER "CN=Microsoft ID Verified CS [AE]OC CA [0-9][0-9], O=Microsoft Corporation, C=US")
endif()
if(NOT DEFINED TASKSMACK_EXPECTED_EKU OR TASKSMACK_EXPECTED_EKU STREQUAL "")
    set(TASKSMACK_EXPECTED_EKU "1.3.6.1.4.1.311.97.*")
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
_ts_quote("${TASKSMACK_EXPECTED_ISSUER}" _ts_issuer)
_ts_quote("${TASKSMACK_EXPECTED_EKU}" _ts_eku)

find_program(_ts_powershell NAMES pwsh powershell REQUIRED)
string(CONCAT _ts_script
    "$ErrorActionPreference='Stop'\n${_ts_check}\n"
    "Test-TaskSmackSignatures -Signatures @(@(${_ts_list}) | ForEach-Object { Get-AuthenticodeSignature -LiteralPath $_ }) "
    "-ExpectedSigner ${_ts_expected} -ExpectedIssuer ${_ts_issuer} -ExpectedEku ${_ts_eku}")
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
                        "${TASKSMACK_EXPECTED_SIGNER} through Azure Artifact Signing (#2302, #2990): sign "
                        "TaskSmack.exe and TaskSmackHelper.exe before packaging, or configure with "
                        "-DTASKSMACK_PACKAGE_WINDOWS_HELPER=OFF.\n${_ts_output}")
endif()
if(NOT _ts_statuses EQUAL 2 OR NOT _ts_verdict STREQUAL "OK")
    message(FATAL_ERROR "VerifyWindowsSignatures: couldn't read both signatures:\n${_ts_output}${_ts_error}")
endif()
message(STATUS "VerifyWindowsSignatures: TaskSmack.exe and TaskSmackHelper.exe are signed and timestamped by "
               "${TASKSMACK_EXPECTED_SIGNER}, with certificates issued by ${TASKSMACK_EXPECTED_ISSUER} that "
               "carry ${TASKSMACK_EXPECTED_EKU}")
