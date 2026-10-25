<#
.SYNOPSIS
    Check the Authenticode signatures of an extracted, signed Windows .zip (#2302).
.DESCRIPTION
    release.yml's signature check: validate-windows-package runs it on sign-windows' signed x64 .zip
    before create-release can publish it, and verify-published-release on the x64 .zip downloaded
    from the release (#2721). Both run it only while signing is on (SIGNING_ENABLED).

    cmake/VerifyWindowsSignatures.cmake requires TaskSmack.exe and TaskSmackHelper.exe to be validly
    signed by the policy's signer (ExpectedSigner), timestamped, with one certificate. A missing,
    invalid, wrongly signed or untimestamped signature fails.

    test-signing's certificate is self-signed, so no machine trusts it and its signatures read as
    UnknownError, not Valid. For that policy only, this first trusts the certificate TaskSmack.exe
    was signed with, once it is self-signed and names ExpectedSigner, in the machine's Root store:
    run it on an ephemeral runner only. The check still proves both files are unchanged since
    SignPath signed them, timestamped and signed by that one certificate. release-signing's SignPath
    Foundation certificate chains to a public root, so nothing is added to the trust store for it.
.PARAMETER PackageRoot
    The extracted .zip's top-level directory, which holds TaskSmack.exe and TaskSmackHelper.exe.
.PARAMETER ExpectedSigner
    The signer certificate's common name (release.yml's WINDOWS_EXPECTED_SIGNER).
.PARAMETER SigningPolicy
    The SignPath signing policy the .zip was signed under (release.yml's SIGNPATH_SIGNING_POLICY).
.PARAMETER VerifyScript
    The path to cmake/VerifyWindowsSignatures.cmake.
#>
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$ExpectedSigner,
    [Parameter(Mandatory)][string]$SigningPolicy,
    [Parameter(Mandatory)][string]$VerifyScript
)

foreach ($name in 'TaskSmack.exe', 'TaskSmackHelper.exe') {
  $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot $name)
  Write-Host "${name}: $($sig.Status); signer $($sig.SignerCertificate.Subject) ($($sig.SignerCertificate.Thumbprint)); timestamped by $($sig.TimeStamperCertificate.Subject)"
}
if ($SigningPolicy -eq 'test-signing') {
  $cert = (Get-AuthenticodeSignature -LiteralPath (Join-Path $PackageRoot 'TaskSmack.exe')).SignerCertificate
  if (-not $cert) {
    Write-Error "TaskSmack.exe isn't signed."
    exit 1
  }
  $cn = $cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
  if ($cn -ne $ExpectedSigner -or $cert.Subject -ne $cert.Issuer) {
    Write-Error "TaskSmack.exe is signed by $($cert.Subject), issued by $($cert.Issuer); test-signing expects SignPath's self-signed '$ExpectedSigner' certificate."
    exit 1
  }
  Write-Host "Trusting the self-signed test certificate on this runner only: $($cert.Subject) ($($cert.Thumbprint))"
  $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('Root', 'LocalMachine')
  $store.Open('ReadWrite')
  try { $store.Add($cert) } finally { $store.Close() }
}
cmake "-DTASKSMACK_VERIFY_DIR=$PackageRoot" "-DTASKSMACK_EXPECTED_SIGNER=$ExpectedSigner" -P $VerifyScript
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
