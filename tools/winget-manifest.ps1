#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Write TaskSmack's WinGet manifests for one release from packaging/winget/ and its SHA256SUMS (#3486).
.DESCRIPTION
    Fills in the three templates in packaging/winget/ (version, defaultLocale and installer)
    for -Version: ${VERSION}, and the SHA-256 of the x64 and ARM64 NSIS installers, read from the release's
    SHA256SUMS as ${SHA256_X64} (TaskSmack-<version>-win64.exe) and ${SHA256_ARM64}
    (TaskSmack-<version>-windows-arm64.exe), and ${RELEASE_DATE} from -ReleaseDate (the line is left out
    without it). They go to <OutDir>\manifests\t\TaskSmack\TaskSmack\<version>\,
    the folder winget-pkgs keeps them in, ready for `winget validate`, `wingetcreate submit` or
    `komac submit`. Nothing is downloaded or installed: the hashes are the ones release.yml published and
    Sigstore-signed. winget-pkgs' validation pipeline, and `winget install --manifest` or SandboxTest.ps1,
    download the installers and check them against these hashes.

    This is for the first submission, made by hand (docs/dev/release-process.md, "Publishing to WinGet").
    Later releases are submitted by release.yml's publish-winget job, which updates the manifests in
    winget-pkgs from the previous version's.
.PARAMETER Version
    The release's version, X.Y.Z with no leading v (e.g. 0.11.0).
.PARAMETER ReleaseDir
    A folder holding the release's SHA256SUMS (gh release download vX.Y.Z --pattern SHA256SUMS). Check it
    against SHA256SUMS.bundle first (docs/guide/user-guide.md, "Verifying a Release").
.PARAMETER ReleaseDate
    The release's date, yyyy-MM-dd (gh release view vX.Y.Z --json publishedAt --jq '.publishedAt[:10]').
    Optional; winget-pkgs doesn't require it.
.PARAMETER OutDir
    Where the manifests\ tree is written. Omitted, <ReleaseDir>\winget. The version's folder must not exist yet.
.EXAMPLE
    gh release download v0.11.0 --repo mgradwohl/tasksmack --pattern SHA256SUMS --dir release
    pwsh tools/winget-manifest.ps1 -Version 0.11.0 -ReleaseDir release -ReleaseDate 2026-12-01
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$ReleaseDir,
    [string]$ReleaseDate = '',
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
$parsedDate = [datetime]::MinValue
if ($ReleaseDate -and -not [datetime]::TryParseExact($ReleaseDate, 'yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::None, [ref]$parsedDate)) {
    Fail "-ReleaseDate must be yyyy-MM-dd, not '$ReleaseDate'."
}
# Absolute from here on: .NET's file calls resolve a relative path against the process's working directory,
# which in a PowerShell session after Set-Location isn't PowerShell's location.
$here = (Get-Location -PSProvider FileSystem).ProviderPath
$ReleaseDir = [IO.Path]::GetFullPath($ReleaseDir, $here)
$sumsPath = Join-Path $ReleaseDir 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $sumsPath -PathType Leaf)) { Fail "$sumsPath not found." }
$OutDir = if ($OutDir) { [IO.Path]::GetFullPath($OutDir, $here) } else { Join-Path $ReleaseDir 'winget' }

# SHA256SUMS is `sha256sum` output: '<64 hex>  <name>' per line (release.yml, "Write SHA256SUMS").
$sums = @{}
foreach ($line in [IO.File]::ReadAllLines($sumsPath)) {
    if ($line -match '^([0-9a-f]{64})  ([^/\\]+)$') { $sums[$Matches[2]] = $Matches[1] }
    elseif ($line.Trim()) { Fail "$sumsPath has a line that isn't '<sha256>  <name>': $line" }
}
$installers = [ordered]@{
    SHA256_X64 = "TaskSmack-$Version-win64.exe"
    SHA256_ARM64 = "TaskSmack-$Version-windows-arm64.exe"
}
$values = @{ VERSION = $Version }
if ($ReleaseDate) { $values['RELEASE_DATE'] = $ReleaseDate }
foreach ($key in $installers.Keys) {
    $name = $installers[$key]
    if (-not $sums.ContainsKey($name)) { Fail "$sumsPath lists no $name; is it v$Version's?" }
    $values[$key] = $sums[$name].ToUpperInvariant()
}

# The schema version the submission tool writes: release.yml's WINGETCREATE_MANIFEST_VERSION, beside the
# wingetcreate pin. wingetcreate validates against its own schema and rewrites what it updates to it, so a manifest
# on any other version would be refused, or rewritten on the first automatic update.
$repoRoot = Split-Path -Parent $PSScriptRoot
$workflow = Join-Path $repoRoot '.github/workflows/release.yml'
$pinned = @([IO.File]::ReadAllLines($workflow) | Where-Object { $_ -match "^\s+WINGETCREATE_MANIFEST_VERSION:\s*'?([0-9]+\.[0-9]+\.[0-9]+)'?\s*$" } |
        ForEach-Object { $Matches[1] })
if ($pinned.Count -ne 1) { Fail "$workflow sets WINGETCREATE_MANIFEST_VERSION $($pinned.Count) times, expected once." }
$manifestVersion = $pinned[0]
$templateDir = Join-Path $repoRoot 'packaging/winget'
$templates = @('TaskSmack.TaskSmack.yaml', 'TaskSmack.TaskSmack.installer.yaml', 'TaskSmack.TaskSmack.locale.en-US.yaml')
$target = Join-Path $OutDir "manifests/t/TaskSmack/TaskSmack/$Version"
if (Test-Path -LiteralPath $target) { Fail "$target already exists; remove it or pick another -OutDir." }

# Every manifest is filled in before any is written, so a failure leaves no partial folder.
$manifests = [ordered]@{}
foreach ($template in $templates) {
    $source = Join-Path $templateDir $template
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Fail "$source not found." }
    # The templates' comments are about the templates, so only the schema line is kept.
    # Without -ReleaseDate, its optional line is left out.
    $lines = @([IO.File]::ReadAllLines($source) | Where-Object { $_ -notmatch '^\s*#(?! yaml-language-server:)' } |
            Where-Object { $ReleaseDate -or -not $_.Contains('${RELEASE_DATE}') })
    $text = ($lines -join "`n").Trim() + "`n"
    foreach ($placeholder in [regex]::Matches($text, '\$\{([A-Z0-9_]+)\}')) {
        $key = $placeholder.Groups[1].Value
        if (-not $values.ContainsKey($key)) { Fail "$template uses `${$key}, which this script doesn't fill in." }
    }
    foreach ($key in $values.Keys) { $text = $text.Replace("`${$key}", $values[$key]) }
    $versions = @([regex]::Matches($text, '(?m)^ManifestVersion: (\S+)$') | ForEach-Object { $_.Groups[1].Value })
    $schemas = @([regex]::Matches($text, '(?m)^# yaml-language-server: \$schema=https://aka\.ms/winget-manifest\.[A-Za-z]+\.(\S+)\.schema\.json$') |
            ForEach-Object { $_.Groups[1].Value })
    if ($versions.Count -ne 1 -or $versions[0] -ne $manifestVersion -or $schemas.Count -ne 1 -or $schemas[0] -ne $manifestVersion) {
        Fail "$template is on ManifestVersion [$($versions -join ', ')] with schema header [$($schemas -join ', ')], but wingetcreate submits $manifestVersion (WINGETCREATE_MANIFEST_VERSION in release.yml); keep them the same."
    }
    $manifests[$template] = $text
}
# Written to a staging folder beside the version's and renamed into place, so a failed write leaves no folder
# (empty or partial) that would block the next run.
$parent = Split-Path -Parent $target
[IO.Directory]::CreateDirectory($parent) | Out-Null
$staging = Join-Path $parent ".$Version.tmp-$([guid]::NewGuid().ToString('N'))"
try {
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    $utf8 = [Text.UTF8Encoding]::new($false)
    foreach ($template in $manifests.Keys) { [IO.File]::WriteAllText((Join-Path $staging $template), $manifests[$template], $utf8) }
    [IO.Directory]::Move($staging, $target)
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "Wrote TaskSmack.TaskSmack $Version's manifests (schema $manifestVersion) to $target"
Write-Host "  x64:   $($installers.SHA256_X64)  $($values.SHA256_X64)"
Write-Host "  arm64: $($installers.SHA256_ARM64)  $($values.SHA256_ARM64)"
Write-Host 'Next:'
Write-Host "  winget validate --manifest `"$target`""
Write-Host "  wingetcreate submit --prtitle `"New package: TaskSmack.TaskSmack version $Version`" `"$target`""
