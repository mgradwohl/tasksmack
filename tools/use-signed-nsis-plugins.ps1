<#
.SYNOPSIS
    Put the release-signed NSIS plugin DLLs in place of the runner's unsigned ones before makensis runs (#4864).
.DESCRIPTION
    Smart App Control refuses to load an unsigned DLL, and an NSIS installer unpacks its plugins (nsExec.dll,
    UserInfo.dll, ...) unsigned into %TEMP%\ns*.tmp and loads them from there: under Smart App Control the
    installer's trust check (nsExec::ExecToStack, cmake/CPackProjectConfig.cmake.in) fails and it exits 87, and
    the uninstaller, which embeds the same plugins, removes nothing. makensis embeds each plugin file as it is,
    so release.yml's sign-nsis-plugins job signs every DLL in the pinned NSIS's Plugins\x86-unicode folder (the
    target `Unicode true` selects) with the release certificate, and each job that runs makensis with signing on
    (build-windows' CPack first pass, which also builds Uninstall.exe; build-windows-installer's second pass; the
    reproducibility rebuild) runs this first, so every installer and uninstaller embeds signed plugins.

    Install (the default): the signed set must name exactly the DLLs in PluginsDir, each runner DLL must be the
    unsigned file the signing job started from (its SHA-256 in the set's manifest, so a different NSIS release
    fails rather than mixing plugins), and each signed DLL must hash as the manifest says and be validly signed by
    ExpectedSigner and timestamped. Only then are the signed DLLs copied over the runner's.

    -Check, after makensis: every DLL in PluginsDir is still the signed one, so what makensis embedded was signed.
.PARAMETER SignedDir
    The downloaded nsis-plugins-signed artifact: the signed DLLs and plugins.json, the signing job's manifest
    ({ "plugins": { "<name>": { "unsigned": "<SHA-256>", "signed": "<SHA-256>" } } }).
.PARAMETER ExpectedSigner
    The signer certificate's common name (release.yml's WINDOWS_EXPECTED_SIGNER).
.PARAMETER PluginsDir
    The NSIS plugin folder makensis reads. Omitted, Plugins\x86-unicode beside $env:MAKENSIS (setup-nsis').
.PARAMETER Check
    Check that PluginsDir holds the signed DLLs; change nothing.
#>
#Requires -Version 7
param(
    [Parameter(Mandatory)][string]$SignedDir,
    [Parameter(Mandatory)][string]$ExpectedSigner,
    [string]$PluginsDir = '',
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    Write-Host "::error title=NSIS plugins::$Message"
    exit 1
}

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant() }

if (-not $PluginsDir) {
    if (-not $env:MAKENSIS) { Fail 'No -PluginsDir and no MAKENSIS (setup-nsis sets it).' }
    $PluginsDir = Join-Path (Split-Path -Parent $env:MAKENSIS) 'Plugins\x86-unicode'
}
if (-not (Test-Path -LiteralPath $PluginsDir -PathType Container)) { Fail "$PluginsDir isn't a folder." }
$manifestPath = Join-Path $SignedDir 'plugins.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { Fail "$manifestPath not found." }

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
if ($manifest -isnot [hashtable] -or -not $manifest.ContainsKey('plugins') -or $manifest['plugins'] -isnot [hashtable] -or
    $manifest['plugins'].Count -eq 0) {
    Fail "$manifestPath has no plugins."
}
# Names compare without case, as Windows and makensis do.
$entries = @{}
foreach ($name in $manifest['plugins'].Keys) {
    $entry = $manifest['plugins'][$name]
    if ($entry -isnot [hashtable] -or -not $entry.ContainsKey('unsigned') -or -not $entry.ContainsKey('signed')) {
        Fail "$manifestPath's entry for $name needs 'unsigned' and 'signed' SHA-256s."
    }
    $entries[$name.ToLowerInvariant()] = @{ Name = $name; Unsigned = "$($entry['unsigned'])".ToUpperInvariant(); Signed = "$($entry['signed'])".ToUpperInvariant() }
}

$runnerDlls = @(Get-ChildItem -LiteralPath $PluginsDir -File -Filter '*.dll')
$runnerNames = @($runnerDlls | ForEach-Object { $_.Name.ToLowerInvariant() } | Sort-Object)
$signedNames = @($entries.Keys | Sort-Object)
if (($runnerNames -join ',') -ne ($signedNames -join ',')) {
    Fail "$PluginsDir holds $($runnerDlls.Name -join ', '), but the signed set names $(($entries.Values | ForEach-Object Name | Sort-Object) -join ', '). Every plugin makensis can embed must be signed: sign-nsis-plugins and this job must use the same NSIS."
}

function Test-Signature([string]$Path, [string]$Label) {
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    $cn = if ($sig.SignerCertificate) { $sig.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) } else { '-' }
    if ($sig.Status -ne 'Valid' -or $cn -ne $ExpectedSigner -or -not $sig.TimeStamperCertificate) {
        Write-Host "::error title=NSIS plugins::$Label isn't validly signed and timestamped by '$ExpectedSigner' ($($sig.Status), signer '$cn': $($sig.StatusMessage))."
        return $false
    }
    return $true
}

$failed = $false
foreach ($dll in $runnerDlls) {
    $entry = $entries[$dll.Name.ToLowerInvariant()]
    $runnerHash = Get-Sha256 $dll.FullName
    if ($Check) {
        if ($runnerHash -ne $entry.Signed) {
            Write-Host "::error title=NSIS plugins::$($dll.FullName) ($runnerHash) isn't the signed $($entry.Name) ($($entry.Signed)): makensis may have embedded an unsigned plugin."
            $failed = $true
        } elseif (Test-Signature $dll.FullName $dll.FullName) {
            Write-Host "$($dll.Name): the signed plugin ($runnerHash)."
        } else {
            $failed = $true
        }
        continue
    }

    $signed = Join-Path $SignedDir $entry.Name
    if (-not (Test-Path -LiteralPath $signed -PathType Leaf)) {
        Write-Host "::error title=NSIS plugins::The signed set has no $($entry.Name) file."
        $failed = $true
        continue
    }
    $signedHash = Get-Sha256 $signed
    # A runner DLL that is already the signed one (a step run twice) needs no copy, but is still checked.
    if ($runnerHash -ne $entry.Unsigned -and $runnerHash -ne $entry.Signed) {
        Write-Host "::error title=NSIS plugins::$($dll.FullName) ($runnerHash) isn't the file sign-nsis-plugins signed ($($entry.Unsigned)): a different NSIS release?"
        $failed = $true
        continue
    }
    if ($signedHash -ne $entry.Signed) {
        Write-Host "::error title=NSIS plugins::The signed $($entry.Name) ($signedHash) isn't the one plugins.json names ($($entry.Signed))."
        $failed = $true
        continue
    }
    if (-not (Test-Signature $signed "The signed $($entry.Name)")) {
        $failed = $true
    }
}
if ($failed) { exit 1 }
if ($Check) {
    Write-Host "All $($runnerDlls.Count) plugins in $PluginsDir are the signed ones."
    exit 0
}

foreach ($dll in $runnerDlls) {
    $entry = $entries[$dll.Name.ToLowerInvariant()]
    if ((Get-Sha256 $dll.FullName) -eq $entry.Signed) {
        Write-Host "$($dll.Name): already the signed plugin."
        continue
    }
    Copy-Item -LiteralPath (Join-Path $SignedDir $entry.Name) -Destination $dll.FullName -Force
    if ((Get-Sha256 $dll.FullName) -ne $entry.Signed) { Fail "Copying the signed $($entry.Name) over $($dll.FullName) didn't take." }
    Write-Host "$($dll.Name): $($entry.Unsigned) -> signed $($entry.Signed)"
}
Write-Host "makensis now embeds the $($runnerDlls.Count) signed plugins in $PluginsDir."
exit 0
