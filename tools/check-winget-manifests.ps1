#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Check a release's WinGet manifests: schema, version, installer URLs and hashes (#3822).
.DESCRIPTION
    release.yml's dry run builds the manifests of the previous published release and checks them here, with no
    token and no submission, so the publish-winget job's manifest step is exercised before a real tag. Checks
    the version folder's three manifests: each one's ManifestVersion (and its schema header, when it has one) is
    -ManifestVersion, PackageIdentifier is TaskSmack.TaskSmack and PackageVersion is -Version; the installer
    manifest lists exactly the x64 and ARM64 NSIS installers at their release URLs for -Tag, and each
    InstallerSha256 equals the SHA-256 of the installer of that name in -InstallerDir. Prints every problem and
    exits 1; exits 0 when there is none. Nothing is downloaded and nothing is submitted.
.PARAMETER ManifestDir
    The version's folder (manifests\t\TaskSmack\TaskSmack\<version>), as winget-manifest.ps1 and
    `wingetcreate update --out` write it.
.PARAMETER Version
    X.Y.Z, no leading v.
.PARAMETER Tag
    The release's tag, vX.Y.Z; its release page's downloads are the installers' URLs.
.PARAMETER Repository
    owner/name of the repository that published the release.
.PARAMETER ManifestVersion
    The schema version release.yml's wingetcreate pin writes (WINGETCREATE_MANIFEST_VERSION).
.PARAMETER InstallerDir
    A folder holding the release's installers, to hash and compare with each InstallerSha256.
#>
param(
    [Parameter(Mandatory)][string]$ManifestDir,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$ManifestVersion,
    [Parameter(Mandatory)][string]$InstallerDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$PackageId = 'TaskSmack.TaskSmack'

# The problems with the manifests in $Directory, each a line of text; empty when there are none.
function Test-WingetManifests([string]$Directory, [string]$Version, [string]$Tag, [string]$Repository,
    [string]$ManifestVersion, [string]$InstallerDir) {
    $problems = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        $problems.Add("$Directory isn't a folder")
        return $problems.ToArray()
    }
    if ((Split-Path -Leaf $Directory) -cne $Version) { $problems.Add("the folder is named $(Split-Path -Leaf $Directory), not $Version") }
    $files = @(Get-ChildItem -LiteralPath $Directory -File -Filter '*.yaml')
    $installerFile = @($files | Where-Object { $_.Name -ceq "$PackageId.installer.yaml" })
    $versionFile = @($files | Where-Object { $_.Name -ceq "$PackageId.yaml" })
    $localeFile = @($files | Where-Object { $_.Name -cmatch "^$([regex]::Escape($PackageId))\.locale\.[A-Za-z-]+\.yaml$" })
    if ($installerFile.Count -ne 1 -or $versionFile.Count -ne 1 -or $localeFile.Count -lt 1) {
        $problems.Add("expected $PackageId.yaml, $PackageId.installer.yaml and a $PackageId.locale.<tag>.yaml, found: $(($files | ForEach-Object Name) -join ', ')")
    }
    foreach ($file in $files) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        $name = $file.Name
        $found = @([regex]::Matches($text, '(?m)^ManifestVersion: (\S+)\s*$') | ForEach-Object { $_.Groups[1].Value })
        if ($found.Count -ne 1 -or $found[0] -cne $ManifestVersion) { $problems.Add("$name is on ManifestVersion [$($found -join ', ')], not $ManifestVersion") }
        $schemas = @([regex]::Matches($text, '(?m)^# yaml-language-server: \$schema=https://aka\.ms/winget-manifest\.[A-Za-z]+\.(\S+)\.schema\.json\s*$') |
                ForEach-Object { $_.Groups[1].Value })
        if ($schemas.Count -gt 0 -and ($schemas.Count -ne 1 -or $schemas[0] -cne $ManifestVersion)) { $problems.Add("$name's schema header is [$($schemas -join ', ')], not $ManifestVersion") }
        $ids = @([regex]::Matches($text, '(?m)^PackageIdentifier: (\S+)\s*$') | ForEach-Object { $_.Groups[1].Value })
        if ($ids.Count -ne 1 -or $ids[0] -cne $PackageId) { $problems.Add("$name's PackageIdentifier is [$($ids -join ', ')], not $PackageId") }
        $versions = @([regex]::Matches($text, '(?m)^PackageVersion: (\S+)\s*$') | ForEach-Object { $_.Groups[1].Value.Trim("'", '"') })
        if ($versions.Count -ne 1 -or $versions[0] -cne $Version) { $problems.Add("$name's PackageVersion is [$($versions -join ', ')], not $Version") }
    }
    if ($installerFile.Count -eq 1) {
        $text = Get-Content -LiteralPath $installerFile[0].FullName -Raw
        $list = [regex]::Match($text, '(?ms)^Installers:\s*\n(.*?)(?=^\S|\z)')
        $entries = if ($list.Success) { @([regex]::Split($list.Groups[1].Value, '(?m)^  - ') | Where-Object { $_.Trim() }) } else { @() }
        $expected = [ordered]@{ x64 = "TaskSmack-$Version-win64.exe"; arm64 = "TaskSmack-$Version-windows-arm64.exe" }
        $seen = @{}
        foreach ($entry in $entries) {
            $arch = [regex]::Match($entry, '(?m)^\s*(?:- )?Architecture: (\S+)\s*$').Groups[1].Value
            $url = [regex]::Match($entry, '(?m)^\s*InstallerUrl: (\S+)\s*$').Groups[1].Value
            $hash = [regex]::Match($entry, '(?m)^\s*InstallerSha256: (\S+)\s*$').Groups[1].Value
            if (-not $expected.Contains($arch)) { $problems.Add("an installer entry has Architecture '$arch', not x64 or arm64"); continue }
            if ($seen.ContainsKey($arch)) { $problems.Add("the $arch installer is listed twice"); continue }
            $seen[$arch] = $true
            $installer = $expected[$arch]
            $wantUrl = "https://github.com/$Repository/releases/download/$Tag/$installer"
            if ($url -cne $wantUrl) { $problems.Add("the $arch InstallerUrl is [$url], not $wantUrl") }
            $path = Join-Path $InstallerDir $installer
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $problems.Add("$installer isn't in $InstallerDir to hash"); continue }
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($hash -notmatch '^[0-9A-Fa-f]{64}$') { $problems.Add("the $arch InstallerSha256 is [$hash], not 64 hex digits") }
            elseif ($hash -ine $actual) { $problems.Add("the $arch InstallerSha256 is $hash, but $installer hashes to $actual") }
        }
        foreach ($arch in $expected.Keys) { if (-not $seen.ContainsKey($arch)) { $problems.Add("the installer manifest lists no $arch installer") } }
    }
    return $problems.ToArray()
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { Write-Error "-Version must be X.Y.Z, not '$Version'."; exit 2 }
    if ($Tag -cne "v$Version") { Write-Error "-Tag must be v$Version, not '$Tag'."; exit 2 }
    $problems = @(Test-WingetManifests $ManifestDir $Version $Tag $Repository $ManifestVersion $InstallerDir)
    if ($problems.Count -gt 0) {
        foreach ($problem in $problems) { Write-Host "::error title=WinGet manifests::$problem" }
        Write-Error "$($problems.Count) problem(s) with the WinGet manifests in ${ManifestDir}:`n$($problems -join "`n")"
        exit 1
    }
    Write-Host "The WinGet manifests in $ManifestDir are on ManifestVersion $ManifestVersion, for $PackageId $Version, with both installers' URLs and SHA-256 matching."
}
