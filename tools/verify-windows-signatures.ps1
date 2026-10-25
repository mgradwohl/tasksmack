<#
.SYNOPSIS
    Check the Authenticode signatures of an extracted, signed Windows .zip (#2302).
.DESCRIPTION
    release.yml's signature check: validate-windows-package runs it on sign-windows' signed .zip
    before create-release can publish it, and verify-published-release on the x64 .zip downloaded
    from the release (#2721). Both run it only while signing is on (SIGNING_ENABLED).

    cmake/VerifyWindowsSignatures.cmake requires TaskSmack.exe and TaskSmackHelper.exe to be validly
    signed by ExpectedSigner, timestamped, and both by a certificate with the same subject (Azure
    Artifact Signing's certificates rotate every few days, so the subject is what stays the same,
    never a thumbprint). A missing, invalid, wrongly signed or untimestamped signature fails. The
    Public Trust certificate chains to a root Windows trusts, so nothing is added to the trust store.
.PARAMETER PackageRoot
    The extracted .zip's top-level directory, which holds TaskSmack.exe and TaskSmackHelper.exe.
.PARAMETER ExpectedSigner
    The signer certificate's common name (release.yml's WINDOWS_EXPECTED_SIGNER).
.PARAMETER VerifyScript
    The path to cmake/VerifyWindowsSignatures.cmake.
#>
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$ExpectedSigner,
    [Parameter(Mandatory)][string]$VerifyScript
)

foreach ($name in 'TaskSmack.exe', 'TaskSmackHelper.exe') {
  $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $name)
  Write-Host "${name}: $($sig.Status); signer $($sig.SignerCertificate.Subject) ($($sig.SignerCertificate.Thumbprint)); issuer $($sig.SignerCertificate.Issuer); timestamped by $($sig.TimeStamperCertificate.Subject)"
}
cmake "-DTASKSMACK_VERIFY_DIR=$PackageRoot" "-DTASKSMACK_EXPECTED_SIGNER=$ExpectedSigner" -P $VerifyScript
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
