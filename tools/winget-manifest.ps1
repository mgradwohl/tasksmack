#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Write TaskSmack's WinGet manifests for one release from packaging/winget/ and its SHA256SUMS (#3486).
.DESCRIPTION
    Fills in the three templates in packaging/winget/ (version, defaultLocale and installer, schema 1.10.0)
    for -Version: ${VERSION}, and the SHA-256 of the x64 and ARM64 NSIS installers, read from the release's
    SHA256SUMS as ${SHA256_X64} (TaskSmack-<version>-win64.exe) and ${SHA256_ARM64}
    (TaskSmack-<version>-windows-arm64.exe). They go to <OutDir>\manifests\t\TaskSmack\TaskSmack\<version>\,
    the folder winget-pkgs keeps them in, ready for `winget validate`, `wingetcreate submit` or
    `komac submit`. Nothing is downloaded or installed: the hashes are the ones release.yml published.

    This is for the first submission, made by hand (docs/dev/release-process.md, "Publishing to WinGet").
    Later releases are submitted by release.yml's publish-winget job, which updates the manifests in
    winget-pkgs from the previous version's.
.PARAMETER Version
    The release's version, X.Y.Z with no leading v (e.g. 0.11.0).
.PARAMETER ReleaseDir
    A folder holding the release's SHA256SUMS (gh release download vX.Y.Z --pattern SHA256SUMS). Check it
    against SHA256SUMS.bundle first (docs/guide/user-guide.md, "Verifying a Release").
.PARAMETER OutDir
    Where the manifests\ tree is written. Omitted, <ReleaseDir>\winget. The version's folder must not exist yet.
.EXAMPLE
    gh release download v0.11.0 --repo mgradwohl/tasksmack --pattern SHA256SUMS --dir release
    pwsh tools/winget-manifest.ps1 -Version 0.11.0 -ReleaseDir release
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$ReleaseDir,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    [Console]::Error.WriteLine("winget-manifest.ps1: $Message")
    exit 1
}

if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    Fail "-Version must be X.Y.Z with no leading v, not '$Version'."
}
$sumsPath = Join-Path $ReleaseDir 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $sumsPath -PathType Leaf)) { Fail "$sumsPath not found." }
if (-not $OutDir) { $OutDir = Join-Path $ReleaseDir 'winget' }

# SHA256SUMS is `sha256sum` output: '<64 hex>  <name>' per line (release.yml, "Write SHA256SUMS").
$sums = @{}
foreach ($line in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $sumsPath).Path)) {
    if ($line -match '^([0-9a-f]{64})  ([^/\\]+)$') { $sums[$Matches[2]] = $Matches[1] }
    elseif ($line.Trim()) { Fail "$sumsPath has a line that isn't '<sha256>  <name>': $line" }
}
$installers = [ordered]@{
    SHA256_X64 = "TaskSmack-$Version-win64.exe"
    SHA256_ARM64 = "TaskSmack-$Version-windows-arm64.exe"
}
$values = @{ VERSION = $Version }
foreach ($key in $installers.Keys) {
    $name = $installers[$key]
    if (-not $sums.ContainsKey($name)) { Fail "$sumsPath lists no $name; is it v$Version's?" }
    $values[$key] = $sums[$name].ToUpperInvariant()
}

$templateDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging/winget'
$templates = @('TaskSmack.TaskSmack.yaml', 'TaskSmack.TaskSmack.installer.yaml', 'TaskSmack.TaskSmack.locale.en-US.yaml')
$target = Join-Path $OutDir "manifests/t/TaskSmack/TaskSmack/$Version"
if (Test-Path -LiteralPath $target) { Fail "$target already exists; remove it or pick another -OutDir." }

# Every manifest is filled in before any is written, so a failure leaves no partial folder.
$manifests = [ordered]@{}
foreach ($template in $templates) {
    $source = Join-Path $templateDir $template
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Fail "$source not found." }
    # The templates' comments are about the templates, so only the schema line is kept.
    $lines = @([IO.File]::ReadAllLines($source) | Where-Object { $_ -notmatch '^\s*#(?! yaml-language-server:)' })
    $text = ($lines -join "`n").Trim() + "`n"
    foreach ($placeholder in [regex]::Matches($text, '\$\{([A-Z0-9_]+)\}')) {
        $key = $placeholder.Groups[1].Value
        if (-not $values.ContainsKey($key)) { Fail "$template uses `${$key}, which this script doesn't fill in." }
    }
    foreach ($key in $values.Keys) { $text = $text.Replace("`${$key}", $values[$key]) }
    $manifests[$template] = $text
}
New-Item -ItemType Directory -Path $target -Force | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($template in $manifests.Keys) { [IO.File]::WriteAllText((Join-Path $target $template), $manifests[$template], $utf8) }

$target = (Resolve-Path -LiteralPath $target).Path
Write-Host "Wrote TaskSmack.TaskSmack $Version's manifests to $target"
Write-Host "  x64:   $($installers.SHA256_X64)  $($values.SHA256_X64)"
Write-Host "  arm64: $($installers.SHA256_ARM64)  $($values.SHA256_ARM64)"
Write-Host 'Next:'
Write-Host "  winget validate --manifest `"$target`""
Write-Host "  wingetcreate submit `"$target`"   (or: komac submit `"$target`")"
