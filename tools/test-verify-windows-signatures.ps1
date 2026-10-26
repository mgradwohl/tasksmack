# Tests for the Authenticode gate's decision in cmake/VerifyWindowsSignatures.cmake (#2550, #2302, #2990):
# Test-TaskSmackSignatures, written out by the script's TASKSMACK_WRITE_SIGNATURE_CHECK mode, called with
# signature objects shaped like Get-AuthenticodeSignature's around certificates made in memory with
# CertificateRequest. Nothing is signed, no certificate store is opened and nothing is trusted, so the
# Status each case passes stands for the chain check Windows would have made.
# Registered in CTest on Windows with PowerShell 7 (TASKSMACK_TEST_CMAKE names the cmake to run);
# can also run directly with pwsh -File.
#Requires -Version 7
using namespace System.Security.Cryptography
using namespace System.Security.Cryptography.X509Certificates
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$repo = Split-Path -Parent $PSScriptRoot
$verifyScript = Join-Path $repo 'cmake/VerifyWindowsSignatures.cmake'
$cmake = if ($env:TASKSMACK_TEST_CMAKE) { $env:TASKSMACK_TEST_CMAKE } else { (Get-Command cmake).Source }

# The defaults the script falls back to are the ones cmake/Options.cmake caches, so a configured build
# and a bare `cmake -P` run pin the same thing.
function Get-Default {
    param([string]$Path, [string]$Name, [string]$Suffix)
    $text = Get-Content -Raw -LiteralPath $Path
    $m = [regex]::Match($text, 'set\(' + $Name + ' "([^"]*)"' + $Suffix)
    Assert-True $m.Success "$Path sets no default for $Name"
    $m.Groups[1].Value
}
$defaults = @{}
foreach ($name in 'TASKSMACK_EXPECTED_SIGNER', 'TASKSMACK_EXPECTED_ISSUER', 'TASKSMACK_EXPECTED_EKU') {
    $script = Get-Default $verifyScript $name '\)'
    $cache = Get-Default (Join-Path $repo 'cmake/Options.cmake') $name '\s+CACHE'
    Assert-True ($script -ceq $cache) "$name defaults differ: [$script] in VerifyWindowsSignatures.cmake, [$cache] in Options.cmake"
    $defaults[$name] = $script
}
Assert-True ($defaults.TASKSMACK_EXPECTED_SIGNER -ceq 'Matt Gradwohl') "Signer default: $($defaults.TASKSMACK_EXPECTED_SIGNER)"

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-verify-signatures-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $checkFile = Join-Path $root 'check.ps1'
    & $cmake "-DTASKSMACK_WRITE_SIGNATURE_CHECK=$checkFile" -P $verifyScript
    Assert-True ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $checkFile)) "cmake didn't write the check ($LASTEXITCODE)"
    . $checkFile

    # A certificate made in memory: self-signed when -Issuer is omitted, else issued by that one (which
    # holds its private key, as CreateSelfSigned's result does). Nothing is stored or trusted.
    function New-TestCertificate {
        param([string]$Subject, [X509Certificate2]$Issuer, [string[]]$Ekus = @(), [switch]$Authority)
        $key = [RSA]::Create(2048)
        $request = [CertificateRequest]::new($Subject, $key, [HashAlgorithmName]::SHA256, [RSASignaturePadding]::Pkcs1)
        $request.CertificateExtensions.Add([X509BasicConstraintsExtension]::new([bool]$Authority, $false, 0, $true))
        $usage = if ($Authority) { [X509KeyUsageFlags]::KeyCertSign } else { [X509KeyUsageFlags]::DigitalSignature }
        $request.CertificateExtensions.Add([X509KeyUsageExtension]::new($usage, $true))
        if ($Ekus.Count) {
            $oids = [OidCollection]::new()
            foreach ($eku in $Ekus) { [void]$oids.Add([Oid]::new($eku)) }
            $request.CertificateExtensions.Add([X509EnhancedKeyUsageExtension]::new($oids, $false))
        }
        # A leaf lasts three days, as Artifact Signing's do, inside its CA's month.
        $from = [DateTimeOffset]::UtcNow.AddDays(-1)
        $until = if ($Authority) { $from.AddDays(30) } else { $from.AddDays(3) }
        if ($Issuer) { return $request.Create($Issuer, $from, $until, [guid]::NewGuid().ToByteArray()) }
        $request.CreateSelfSigned($from, $until)
    }

    # Get-AuthenticodeSignature's shape: what the check reads.
    function New-Signature {
        param([string]$Name, [X509Certificate2]$Signer, [string]$Status = 'Valid', [switch]$NoTimestamp)
        [pscustomobject]@{
            Path                   = Join-Path $root $Name
            Status                 = $Status
            SignerCertificate      = $Signer
            TimeStamperCertificate = if ($NoTimestamp) { $null } else { $tsa }
        }
    }

    function Invoke-Check {
        param($App, $Helper, [string]$Issuer = $defaults.TASKSMACK_EXPECTED_ISSUER, [string]$Eku = $defaults.TASKSMACK_EXPECTED_EKU)
        $lines = @(Test-TaskSmackSignatures -Signatures @($App, $Helper) -ExpectedSigner $defaults.TASKSMACK_EXPECTED_SIGNER `
                -ExpectedIssuer $Issuer -ExpectedEku $Eku)
        Assert-True (@($lines | Where-Object { $_ -like 'STATUS TaskSmack*.exe *' }).Count -eq 2) "No status line per file: $($lines -join ' | ')"
        $lines[-1]
    }

    function Assert-Verdict {
        param([string]$Case, [string]$Verdict, [string]$Expected)
        Assert-True ($Verdict -like $Expected) "${Case}: [$Verdict], expected [$Expected]"
        Write-Host "  ${Case}: $Verdict"
    }

    $publisher = 'CN=Matt Gradwohl, O=Matt Gradwohl, L=REDMOND, S=wa, C=US'
    $signingEku = '1.3.6.1.5.5.7.3.3'
    # What the dry run's Artifact Signing leaf carries (#2990): Public Trust, Code Signing, and the
    # identity validation's own usage.
    $azureEkus = @('1.3.6.1.4.1.311.97.1.0', $signingEku, '1.3.6.1.4.1.311.97.703777433.107619563.194876341.588498515')
    $tsa = New-TestCertificate -Subject 'CN=Test Time Stamping Authority' -Ekus '1.3.6.1.5.5.7.3.8'
    $ca04 = New-TestCertificate -Subject 'CN=Microsoft ID Verified CS EOC CA 04, O=Microsoft Corporation, C=US' -Authority
    $ca05 = New-TestCertificate -Subject 'CN=Microsoft ID Verified CS EOC CA 05, O=Microsoft Corporation, C=US' -Authority
    $aoc01 = New-TestCertificate -Subject 'CN=Microsoft ID Verified CS AOC CA 01, O=Microsoft Corporation, C=US' -Authority
    $azure = New-TestCertificate -Subject $publisher -Issuer $ca04 -Ekus $azureEkus
    Assert-True ($azure.Issuer -ceq 'CN=Microsoft ID Verified CS EOC CA 04, O=Microsoft Corporation, C=US') "Test issuer reads [$($azure.Issuer)]"

    # ── Artifact Signing's certificates pass, across rotations and a CA rollover ────────────────────
    Assert-Verdict 'Artifact Signing, one certificate' (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $azure)) 'OK'
    $rotated = New-TestCertificate -Subject $publisher -Issuer $ca05 -Ekus $azureEkus
    Assert-Verdict 'A rotation between the files, onto CA 05' (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $rotated)) 'OK'
    $aoc = New-TestCertificate -Subject $publisher -Issuer $aoc01 -Ekus $azureEkus
    Assert-Verdict 'An AOC CA' (Invoke-Check (New-Signature TaskSmack.exe $aoc) (New-Signature TaskSmackHelper.exe $aoc)) 'OK'
    $publicOnly = New-TestCertificate -Subject $publisher -Issuer $ca04 -Ekus @('1.3.6.1.4.1.311.97.1.0', $signingEku)
    Assert-Verdict 'Only the Public Trust usage' (Invoke-Check (New-Signature TaskSmack.exe $publicOnly) (New-Signature TaskSmackHelper.exe $publicOnly)) 'OK'

    # ── #2990: a trusted certificate that only says CN=Matt Gradwohl is refused ──────────────────────
    # The issue's repro: New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Matt Gradwohl'.
    $selfSigned = New-TestCertificate -Subject 'CN=Matt Gradwohl' -Ekus $signingEku
    Assert-Verdict 'Self-signed CN=Matt Gradwohl' (Invoke-Check (New-Signature TaskSmack.exe $selfSigned) (New-Signature TaskSmackHelper.exe $selfSigned)) `
        "FAIL TaskSmack.exe's signer certificate is issued by CN=Matt Gradwohl, not by *"
    $selfSignedEkus = New-TestCertificate -Subject $publisher -Ekus $azureEkus
    Assert-Verdict "Self-signed, with Artifact Signing's usages" (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $selfSignedEkus)) `
        "FAIL TaskSmackHelper.exe's signer certificate is issued by $publisher, not by *"
    $contosoCa = New-TestCertificate -Subject 'CN=Contoso Code Signing CA, O=Contoso, C=US' -Authority
    $internal = New-TestCertificate -Subject $publisher -Issuer $contosoCa -Ekus $azureEkus
    Assert-Verdict 'An internal CA' (Invoke-Check (New-Signature TaskSmack.exe $internal) (New-Signature TaskSmackHelper.exe $azure)) `
        "FAIL TaskSmack.exe's signer certificate is issued by CN=Contoso Code Signing CA, O=Contoso, C=US, not by *"
    $lookalikeCa = New-TestCertificate -Subject 'CN=Microsoft ID Verified CS EOC CA 04, O=Contoso, C=US' -Authority
    $lookalike = New-TestCertificate -Subject $publisher -Issuer $lookalikeCa -Ekus $azureEkus
    Assert-Verdict 'A lookalike CA name' (Invoke-Check (New-Signature TaskSmack.exe $lookalike) (New-Signature TaskSmackHelper.exe $lookalike)) `
        "FAIL TaskSmack.exe's signer certificate is issued by CN=Microsoft ID Verified CS EOC CA 04, O=Contoso, C=US, not by *"
    $codeSigningOnly = New-TestCertificate -Subject $publisher -Issuer $ca04 -Ekus $signingEku
    Assert-Verdict 'No Artifact Signing usage' (Invoke-Check (New-Signature TaskSmack.exe $codeSigningOnly) (New-Signature TaskSmackHelper.exe $codeSigningOnly)) `
        "FAIL TaskSmack.exe's signer certificate has no 1.3.6.1.4.1.311.97.* extended key usage (it has $signingEku)"
    $noEku = New-TestCertificate -Subject $publisher -Issuer $ca04
    Assert-Verdict 'No extended key usage at all' (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $noEku)) `
        "FAIL TaskSmackHelper.exe's signer certificate has no * extended key usage (it has none)"

    # ── The checks from before #2990 still refuse ───────────────────────────────────────────────────
    $otherName = New-TestCertificate -Subject 'CN=Someone Else, O=Someone Else, C=US' -Issuer $ca04 -Ekus $azureEkus
    Assert-Verdict 'Another signer' (Invoke-Check (New-Signature TaskSmack.exe $otherName) (New-Signature TaskSmackHelper.exe $azure)) `
        'FAIL TaskSmack.exe is signed by Someone Else, not Matt Gradwohl'
    Assert-Verdict 'No timestamp' (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $azure -NoTimestamp)) `
        "FAIL TaskSmackHelper.exe's signature has no timestamp"
    Assert-Verdict 'Changed since signing' (Invoke-Check (New-Signature TaskSmack.exe $azure -Status HashMismatch) (New-Signature TaskSmackHelper.exe $azure)) `
        "FAIL TaskSmack.exe isn't Authenticode-signed (HashMismatch)"
    Assert-Verdict 'Unsigned' (Invoke-Check (New-Signature TaskSmack.exe $null -Status NotSigned) (New-Signature TaskSmackHelper.exe $azure)) `
        "FAIL TaskSmack.exe isn't Authenticode-signed (NotSigned)"
    $otherSubject = New-TestCertificate -Subject 'CN=Matt Gradwohl, O=Matt Gradwohl, C=US' -Issuer $ca04 -Ekus $azureEkus
    Assert-Verdict 'Different subjects' (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $otherSubject)) `
        "FAIL TaskSmack.exe and TaskSmackHelper.exe aren't signed by the same subject*"

    # ── The patterns are the configured ones, not built in ──────────────────────────────────────────
    Assert-Verdict 'A configured issuer' (Invoke-Check (New-Signature TaskSmack.exe $internal) (New-Signature TaskSmackHelper.exe $internal) -Issuer 'CN=Contoso Code Signing CA, O=Contoso, C=US') 'OK'
    Assert-Verdict 'A configured usage' (Invoke-Check (New-Signature TaskSmack.exe $azure) (New-Signature TaskSmackHelper.exe $azure) -Eku '1.2.3.4') `
        "FAIL TaskSmack.exe's signer certificate has no 1.2.3.4 extended key usage*"

    Write-Host 'VerifyWindowsSignatures.cmake check tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
