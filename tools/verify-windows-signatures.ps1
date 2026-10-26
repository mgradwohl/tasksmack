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
    never a thumbprint). The certificate must also come from Artifact Signing (#2990): its issuer
    like ExpectedIssuer and one extended key usage like ExpectedEku. A missing, invalid, wrongly
    signed or untimestamped signature fails. The Public Trust certificate chains to a root Windows
    trusts, so nothing is added to the trust store.
.PARAMETER PackageRoot
    The extracted .zip's top-level directory, which holds TaskSmack.exe and TaskSmackHelper.exe.
.PARAMETER ExpectedSigner
    The signer certificate's common name (release.yml's WINDOWS_EXPECTED_SIGNER).
.PARAMETER ExpectedIssuer
    Optional: a -like pattern for the signer certificate's issuer. Omitted, the script's default
    (TASKSMACK_EXPECTED_ISSUER: Microsoft's ID Verified code-signing CAs).
.PARAMETER ExpectedEku
    Optional: a -like pattern one of the signer certificate's extended key usages must match.
    Omitted, the script's default (TASKSMACK_EXPECTED_EKU: Artifact Signing's 1.3.6.1.4.1.311.97.*).
.PARAMETER VerifyScript
    The path to cmake/VerifyWindowsSignatures.cmake.
#>
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$ExpectedSigner,
    [Parameter(Mandatory)][string]$VerifyScript,
    [string]$ExpectedIssuer = '',
    [string]$ExpectedEku = ''
)

foreach ($name in 'TaskSmack.exe', 'TaskSmackHelper.exe') {
  $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $name)
  $ekus = ($sig.SignerCertificate.EnhancedKeyUsageList | ForEach-Object { $_.ObjectId }) -join ', '
  Write-Host "${name}: $($sig.Status); signer $($sig.SignerCertificate.Subject) ($($sig.SignerCertificate.Thumbprint)); issuer $($sig.SignerCertificate.Issuer); EKUs $ekus; timestamped by $($sig.TimeStamperCertificate.Subject)"
}
cmake "-DTASKSMACK_VERIFY_DIR=$PackageRoot" "-DTASKSMACK_EXPECTED_SIGNER=$ExpectedSigner" `
  "-DTASKSMACK_EXPECTED_ISSUER=$ExpectedIssuer" "-DTASKSMACK_EXPECTED_EKU=$ExpectedEku" -P $VerifyScript
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
