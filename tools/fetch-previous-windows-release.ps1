#Requires -Version 7
#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Download and verify the last published release's Windows installers, for the upgrade check (#3757).
.DESCRIPTION
    release.yml's fetch-previous-windows-release job runs this on Linux, where cosign-installer installs
    cosign, before validate-windows-upgrade installs this run's installers over the ones it downloads
    (tools/windows-upgrade-check.ps1).

    The previous release is the published one (not a draft or a pre-release) with the highest vX.Y.Z tag
    below -Version, the version being built: v0.10.0 for a 0.11.0 build, and v0.10.1 for a 0.10.2 build on
    release/0.10 even once v0.11.0 is out, so the check is always an upgrade, never a downgrade. When there
    is none, or it ships no Windows installer (TaskSmack-<version>-win64.exe, -windows-arm64.exe), it says
    so with a ::notice:: and writes found=false: there is nothing to upgrade from, and the check is skipped.

    Otherwise it downloads the installers it ships, SHA256SUMS and SHA256SUMS.bundle with gh release download,
    and checks them as the User Guide's "Verifying a Release" has users do: cosign verify-blob on the bundle,
    with the certificate pinned to this repository's release.yml at the release's own tag and to GitHub's
    OIDC issuer, then each installer against its SHA256SUMS line. Any failure exits 1. A release with an
    installer but no SHA256SUMS or bundle fails too, since it can't be verified that way. The installers'
    Authenticode signatures are checked on Windows, by validate-windows-upgrade.

    Writes found, tag and version to $env:GITHUB_OUTPUT when it is set, and previous-release.json (the
    same three, and the installers' names) beside the downloads.
.PARAMETER Version
    The version being built, X.Y.Z (CMakeLists.txt's PROJECT_VERSION).
.PARAMETER Repo
    owner/name of the repository whose releases to read. Omitted, $env:GITHUB_REPOSITORY.
.PARAMETER OutDir
    The folder to download into, which must not exist yet or be empty.
.PARAMETER Cosign
    The cosign executable. Omitted, cosign from PATH.
.EXAMPLE
    pwsh tools/fetch-previous-windows-release.ps1 -Version 0.11.0 -Repo mgradwohl/tasksmack -OutDir previous
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Repo = $env:GITHUB_REPOSITORY,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$Cosign = 'cosign'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# A strict vX.Y.Z tag (no leading zeroes, as release.yml's validate requires) as a [version], else $null.
function ConvertTo-ReleaseVersion([string]$Tag) {
    if ($Tag -cmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        return [version]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
    }
    return $null
}

# The release to upgrade from: of $Releases (objects with tag_name, draft, prerelease and assets, an array of
# asset names), the published, non-pre-release one with the highest strict vX.Y.Z tag below $Version, or
# $null when there is none.
function Select-PreviousRelease([object[]]$Releases, [version]$Version) {
    $best = $null
    $bestVersion = $null
    foreach ($release in $Releases) {
        if ($release.draft -or $release.prerelease) { continue }
        $releaseVersion = ConvertTo-ReleaseVersion $release.tag_name
        if ($null -eq $releaseVersion -or $releaseVersion -ge $Version) { continue }
        if ($null -eq $bestVersion -or $releaseVersion -gt $bestVersion) {
            $best = $release
            $bestVersion = $releaseVersion
        }
    }
    return $best
}

# The Windows installers a release ships, of the two release.yml builds: TaskSmack-<version>-win64.exe (x64)
# and TaskSmack-<version>-windows-arm64.exe (ARM64), by asset name.
function Get-WindowsInstallerNames([string[]]$AssetNames, [string]$ReleaseVersion) {
    return , @(foreach ($suffix in 'win64', 'windows-arm64') {
            $name = "TaskSmack-$ReleaseVersion-$suffix.exe"
            if ($AssetNames -ccontains $name) { $name }
        })
}

# The problems with $Names in $Dir against the SHA256SUMS there ('<sha256>  <name>' lines, as sha256sum
# writes them): a name with no line, more than one, or another SHA-256. Empty when every one matches.
function Test-AgainstSha256Sums([string]$Dir, [string[]]$Names) {
    $sums = @{}
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath (Join-Path $Dir 'SHA256SUMS')) {
        if (-not $line.Trim()) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64}) [ *](.+)$') {
            $problems.Add("SHA256SUMS has a line that isn't '<sha256>  <name>': $line")
            continue
        }
        $name = $Matches[2]
        if ($sums.ContainsKey($name)) { $problems.Add("SHA256SUMS lists $name more than once") }
        $sums[$name] = $Matches[1].ToLowerInvariant()
    }
    foreach ($name in $Names) {
        if (-not $sums.ContainsKey($name)) {
            $problems.Add("SHA256SUMS has no line for $name")
            continue
        }
        $actual = (Get-FileHash -LiteralPath (Join-Path $Dir $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $sums[$name]) { $problems.Add("$name has SHA-256 $actual, but SHA256SUMS lists $($sums[$name])") }
    }
    return , $problems.ToArray()
}

# The certificate identity cosign must find in SHA256SUMS.bundle: release.yml in $Repo at the tag's own ref.
function Get-CosignIdentity([string]$Repo, [string]$Tag) {
    return "^https://github\.com/$([regex]::Escape($Repo))/\.github/workflows/release\.yml@refs/tags/$([regex]::Escape($Tag))$"
}

function Set-StepOutput([string]$Name, [string]$Value) {
    if ($env:GITHUB_OUTPUT) { "$Name=$Value" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append }
}

# tools/test-windows-upgrade-check.ps1 loads the functions above from this script's AST; nothing below runs there.

function Fail([string]$Message) {
    Write-Host "::error::fetch-previous-windows-release.ps1: $Message"
    exit 1
}

$buildVersion = ConvertTo-ReleaseVersion "v$Version"
if ($null -eq $buildVersion) { Fail "-Version must be X.Y.Z with no leading v, not '$Version'." }
if (-not $Repo) { Fail 'No -Repo, and GITHUB_REPOSITORY is unset.' }
if ((Test-Path -LiteralPath $OutDir) -and @(Get-ChildItem -LiteralPath $OutDir -Force).Count) { Fail "-OutDir $OutDir isn't empty." }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath

# Every release, a page of 100 at a time, as one JSON object per line.
$lines = & gh api --paginate "repos/$Repo/releases?per_page=100" --jq '.[] | {tag_name, draft, prerelease, assets: [.assets[].name]}'
if ($LASTEXITCODE -ne 0) { Fail "gh api couldn't list $Repo's releases (exit $LASTEXITCODE)." }
$releases = @($lines | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
Write-Host "$Repo has $($releases.Count) release(s); building $Version."

$previous = Select-PreviousRelease $releases $buildVersion
if ($null -eq $previous) {
    Write-Host "::notice::No published release of $Repo is below $Version, so there is no Windows installer to upgrade from: the upgrade check is skipped."
    Set-StepOutput 'found' 'false'
    exit 0
}
$tag = $previous.tag_name
$previousVersion = $tag.Substring(1)
$installers = Get-WindowsInstallerNames @($previous.assets) $previousVersion
if ($installers.Count -eq 0) {
    Write-Host "::notice::$tag, the last published release below $Version, ships no Windows installer (TaskSmack-$previousVersion-win64.exe or -windows-arm64.exe): the upgrade check is skipped."
    Set-StepOutput 'found' 'false'
    exit 0
}
foreach ($required in 'SHA256SUMS', 'SHA256SUMS.bundle') {
    if (@($previous.assets) -cnotcontains $required) { Fail "$tag ships $($installers -join ', ') but no $required, so they can't be verified as the User Guide's 'Verifying a Release' does." }
}
Write-Host "Upgrading from $tag, which ships $($installers -join ' and ')."

$patterns = @($installers) + @('SHA256SUMS', 'SHA256SUMS.bundle') | ForEach-Object { '--pattern'; $_ }
& gh release download $tag --repo $Repo --dir $OutDir @patterns
if ($LASTEXITCODE -ne 0) { Fail "gh release download $tag failed (exit $LASTEXITCODE)." }

$identity = Get-CosignIdentity $Repo $tag
Write-Host "cosign verify-blob SHA256SUMS: certificate identity $identity, OIDC issuer https://token.actions.githubusercontent.com"
& $Cosign verify-blob --bundle (Join-Path $OutDir 'SHA256SUMS.bundle') --certificate-identity-regexp $identity `
    --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' (Join-Path $OutDir 'SHA256SUMS')
if ($LASTEXITCODE -ne 0) { Fail "$tag's SHA256SUMS.bundle doesn't verify (cosign exit $LASTEXITCODE)." }

$problems = Test-AgainstSha256Sums $OutDir $installers
if ($problems.Count) { Fail "$tag's installers don't match its SHA256SUMS:`n$($problems -join "`n")" }
foreach ($name in $installers) { Write-Host "${name}: OK (SHA256SUMS)" }

@{ tag = $tag; version = $previousVersion; installers = @($installers) } | ConvertTo-Json |
    Out-File -FilePath (Join-Path $OutDir 'previous-release.json') -Encoding utf8
Set-StepOutput 'found' 'true'
Set-StepOutput 'tag' $tag
Set-StepOutput 'version' $previousVersion
if ($env:GITHUB_STEP_SUMMARY) {
    "### Previous Windows release`n`n- $tag`: $($installers -join ', '), Sigstore signature of SHA256SUMS and SHA-256s verified" |
        Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
}
Write-Host "$tag's $($installers -join ' and ') are verified against its Sigstore-signed SHA256SUMS."
