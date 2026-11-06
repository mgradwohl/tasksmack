# Tests for tools/winget-manifest.ps1, which fills in packaging/winget/'s manifest templates from a release's
# SHA256SUMS (#3486). Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
# Nothing is downloaded or installed, and winget isn't needed.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script = Join-Path $PSScriptRoot 'winget-manifest.ps1'
# The schema version release.yml's wingetcreate pin submits; every generated manifest must be on it.
$releaseYml = Join-Path (Split-Path -Parent $PSScriptRoot) '.github/workflows/release.yml'
$pinnedVersion = @(Get-Content -LiteralPath $releaseYml | Where-Object { $_ -match "^\s+WINGETCREATE_MANIFEST_VERSION:\s*'([0-9.]+)'" } | ForEach-Object { $Matches[1] })
if ($pinnedVersion.Count -ne 1) { throw "release.yml sets WINGETCREATE_MANIFEST_VERSION $($pinnedVersion.Count) times" }
$pinnedVersion = $pinnedVersion[0]
$pwsh = (Get-Process -Id $PID).Path
$x64 = 'a' * 64
$arm64 = '0123456789abcdef' * 4

function Invoke-Manifest {
    param([string[]]$Arguments)
    $output = & $pwsh -NoProfile -File $script @Arguments 2>&1 | Out-String
    [pscustomobject]@{ Code = $LASTEXITCODE; Output = $output }
}

function New-Release {
    param([string]$Name, [string[]]$Lines)
    $dir = Join-Path $root $Name
    New-Item -ItemType Directory -Path $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'SHA256SUMS'), (($Lines -join "`n") + "`n"))
    $dir
}

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-winget-manifest-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # ── A release's SHA256SUMS fills in every placeholder, in winget-pkgs' folder layout ─────────────
    $release = New-Release 'good' @(
        "$('1' * 64)  TaskSmack-1.2.3-Linux.deb"
        "$x64  TaskSmack-1.2.3-win64.exe"
        "$('2' * 64)  TaskSmack-1.2.3-win64.zip"
        "$arm64  TaskSmack-1.2.3-windows-arm64.exe"
    )
    $run = Invoke-Manifest @('-Version', '1.2.3', '-ReleaseDir', $release, '-ReleaseDate', '2026-12-01')
    Assert-True ($run.Code -eq 0) "A good release failed ($($run.Code)): $($run.Output)"
    $dir = Join-Path $release 'winget/manifests/t/TaskSmack/TaskSmack/1.2.3'
    $names = @(Get-ChildItem -LiteralPath $dir -File | ForEach-Object Name | Sort-Object)
    Assert-True (($names -join ',') -ceq 'TaskSmack.TaskSmack.installer.yaml,TaskSmack.TaskSmack.locale.en-US.yaml,TaskSmack.TaskSmack.yaml') "Manifests written: $($names -join ', ')"
    foreach ($name in $names) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $dir $name))
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        Assert-True (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) "$name starts with a BOM"
        Assert-True (-not $text.Contains("`r")) "$name has CR line endings"
        Assert-True (-not $text.Contains('${')) "$name keeps a placeholder: $text"
        Assert-True ($text -match '(?m)^PackageIdentifier: TaskSmack\.TaskSmack$') "${name}: no PackageIdentifier"
        Assert-True ($text -match '(?m)^PackageVersion: 1\.2\.3$') "${name}: no PackageVersion 1.2.3"
        Assert-True ($text -match "(?m)^ManifestVersion: $([regex]::Escape($pinnedVersion))`$") "${name}: no ManifestVersion $pinnedVersion"
        # Only the schema comment is kept; the templates' own notes stay in packaging/winget/.
        $comments = @($text -split "`n" | Where-Object { $_ -match '^\s*#' })
        Assert-True ($comments.Count -eq 1 -and $comments[0] -like "# yaml-language-server: `$schema=https://aka.ms/winget-manifest.*.$pinnedVersion.schema.json") "$name comments: $($comments -join ' | ')"
    }
    $installer = [IO.File]::ReadAllText((Join-Path $dir 'TaskSmack.TaskSmack.installer.yaml'))
    foreach ($expected in @(
            'InstallerUrl: https://github.com/mgradwohl/tasksmack/releases/download/v1.2.3/TaskSmack-1.2.3-win64.exe'
            "InstallerSha256: $($x64.ToUpperInvariant())"
            'InstallerUrl: https://github.com/mgradwohl/tasksmack/releases/download/v1.2.3/TaskSmack-1.2.3-windows-arm64.exe'
            "InstallerSha256: $($arm64.ToUpperInvariant())"
            'InstallerType: nullsoft'
            'Scope: machine'
            'ElevationRequirement: elevationRequired'
            'ReleaseDate: 2026-12-01'
            'UpgradeBehavior: install'
        )) {
        Assert-True ($installer.Contains($expected)) "The installer manifest lacks '$expected':`n$installer"
    }
    # Each architecture gets its own installer's hash, not the other's.
    Assert-True ($installer -match '(?s)Architecture: x64\s+InstallerUrl: \S+-win64\.exe\s+InstallerSha256: A{64}\s+- Architecture: arm64\s+InstallerUrl: \S+-windows-arm64\.exe\s+InstallerSha256: 0123456789ABCDEF') "Architectures and hashes paired wrongly:`n$installer"
    $locale = [IO.File]::ReadAllText((Join-Path $dir 'TaskSmack.TaskSmack.locale.en-US.yaml'))
    Assert-True ($locale.Contains('ReleaseNotesUrl: https://github.com/mgradwohl/tasksmack/releases/tag/v1.2.3')) "ReleaseNotesUrl:`n$locale"

    # ── An existing version folder isn't overwritten ──────────────────────────────────────────────
    $again = Invoke-Manifest @('-Version', '1.2.3', '-ReleaseDir', $release)
    Assert-True ($again.Code -ne 0 -and $again.Output -match 'already exists') "A second run over the same folder: $($again.Code) $($again.Output)"

    # ── -OutDir puts the tree elsewhere ───────────────────────────────────────────────────────────
    $out = Join-Path $root 'out'
    $run = Invoke-Manifest @('-Version', '1.2.3', '-ReleaseDir', $release, '-OutDir', $out)
    Assert-True ($run.Code -eq 0 -and (Test-Path -LiteralPath (Join-Path $out 'manifests/t/TaskSmack/TaskSmack/1.2.3/TaskSmack.TaskSmack.yaml'))) "-OutDir: $($run.Code) $($run.Output)"
    # Without -ReleaseDate, the optional ReleaseDate line is left out rather than left as a placeholder.
    $undated = [IO.File]::ReadAllText((Join-Path $out 'manifests/t/TaskSmack/TaskSmack/1.2.3/TaskSmack.TaskSmack.installer.yaml'))
    Assert-True (-not $undated.Contains('ReleaseDate') -and -not $undated.Contains('${')) "Without -ReleaseDate:`n$undated"

    # ── Relative paths, run in this session after Set-Location (PRRT_kwDOQo1RlM60WbQG) ────────────
    # In-process, with .NET's working directory somewhere else, as in a PowerShell session whose location
    # changed: the script must resolve -ReleaseDir and -OutDir against PowerShell's location before its .NET
    # file writes, and leave no staging folder behind.
    $relative = Join-Path $root 'relative'
    New-Item -ItemType Directory -Path (Join-Path $relative 'release') | Out-Null
    Copy-Item -LiteralPath (Join-Path $release 'SHA256SUMS') -Destination (Join-Path $relative 'release')
    $savedLocation = Get-Location
    $savedDirectory = [Environment]::CurrentDirectory
    try {
        Set-Location -LiteralPath $relative
        [Environment]::CurrentDirectory = [IO.Path]::GetTempPath()
        & $script -Version 1.2.3 -ReleaseDir 'release' -OutDir 'out' 6>&1 | Out-Null
    } finally {
        Set-Location $savedLocation
        [Environment]::CurrentDirectory = $savedDirectory
    }
    $relativeDir = Join-Path $relative 'out/manifests/t/TaskSmack/TaskSmack'
    $written = @(Get-ChildItem -LiteralPath (Join-Path $relativeDir '1.2.3') -File | ForEach-Object Name)
    Assert-True ($written.Count -eq 3) "Relative paths: wrote [$($written -join ', ')] under $relativeDir"
    $leftovers = @(Get-ChildItem -LiteralPath $relativeDir -Force | Where-Object Name -NE '1.2.3')
    Assert-True ($leftovers.Count -eq 0) "Relative paths: left behind $(($leftovers | ForEach-Object Name) -join ', ')"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) 'out/manifests/t/TaskSmack/TaskSmack/1.2.3'))) 'Relative paths: wrote under .NET''s working directory'
    $staged = @(Get-ChildItem -LiteralPath (Join-Path $release 'winget/manifests/t/TaskSmack/TaskSmack') -Force | Where-Object Name -NE '1.2.3')
    Assert-True ($staged.Count -eq 0) "The good run left $(($staged | ForEach-Object Name) -join ', ') beside its folder"
    # ── Templates on another schema than the one wingetcreate submits are refused ─────────────────
    # A copy of the script, its templates (moved to 1.28.0) and a release.yml naming 1.12.0, as wingetcreate
    # v1.12.13.0 writes: the script must refuse rather than write manifests wingetcreate would reject.
    $fake = Join-Path $root 'fake-repo'
    foreach ($sub in 'tools', 'packaging/winget', '.github/workflows') { New-Item -ItemType Directory -Path (Join-Path $fake $sub) -Force | Out-Null }
    Copy-Item -LiteralPath $script -Destination (Join-Path $fake 'tools')
    foreach ($template in Get-ChildItem -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging/winget') -Filter '*.yaml') {
        $moved = [IO.File]::ReadAllText($template.FullName).Replace("ManifestVersion: $pinnedVersion", 'ManifestVersion: 1.28.0').Replace(".$pinnedVersion.schema.json", '.1.28.0.schema.json')
        [IO.File]::WriteAllText((Join-Path $fake "packaging/winget/$($template.Name)"), $moved)
    }
    [IO.File]::WriteAllText((Join-Path $fake '.github/workflows/release.yml'), "env:`n      WINGETCREATE_MANIFEST_VERSION: '1.12.0'`n")
    $mismatch = New-Release 'mismatch' @("$x64  TaskSmack-1.2.3-win64.exe", "$arm64  TaskSmack-1.2.3-windows-arm64.exe")
    $output = & $pwsh -NoProfile -File (Join-Path $fake 'tools/winget-manifest.ps1') -Version 1.2.3 -ReleaseDir $mismatch 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -ne 0 -and $output -match 'ManifestVersion \[1\.28\.0\].*wingetcreate submits 1\.12\.0') "A 1.28.0 template against wingetcreate's 1.12.0: $LASTEXITCODE $output"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $mismatch 'winget'))) 'A schema mismatch wrote manifests anyway'
    # ── Refusals: a bad version, a missing installer, a malformed line, no SHA256SUMS ────────────────
    foreach ($case in @(
            @{ Name = 'v-prefix'; Version = 'v1.2.3'; Lines = @("$x64  TaskSmack-1.2.3-win64.exe", "$arm64  TaskSmack-1.2.3-windows-arm64.exe"); Expect = 'X\.Y\.Z' }
            @{ Name = 'prerelease'; Version = '1.2.3-rc1'; Lines = @("$x64  TaskSmack-1.2.3-rc1-win64.exe", "$arm64  TaskSmack-1.2.3-rc1-windows-arm64.exe"); Expect = 'X\.Y\.Z' }
            @{ Name = 'bad-date'; Version = '1.2.3'; Date = '12/01/2026'; Lines = @("$x64  TaskSmack-1.2.3-win64.exe", "$arm64  TaskSmack-1.2.3-windows-arm64.exe"); Expect = 'yyyy-MM-dd' }
            @{ Name = 'no-arm64'; Version = '1.2.3'; Lines = @("$x64  TaskSmack-1.2.3-win64.exe"); Expect = 'windows-arm64\.exe' }
            @{ Name = 'other-version'; Version = '1.2.4'; Lines = @("$x64  TaskSmack-1.2.3-win64.exe", "$arm64  TaskSmack-1.2.3-windows-arm64.exe"); Expect = '1\.2\.4-win64\.exe' }
            @{ Name = 'malformed'; Version = '1.2.3'; Lines = @("$x64 TaskSmack-1.2.3-win64.exe", "$arm64  TaskSmack-1.2.3-windows-arm64.exe"); Expect = "isn't '<sha256>  <name>'" }
        )) {
        $dir = New-Release $case.Name $case.Lines
        $arguments = @('-Version', $case.Version, '-ReleaseDir', $dir)
        if ($case.ContainsKey('Date')) { $arguments += @('-ReleaseDate', $case.Date) }
        $run = Invoke-Manifest $arguments
        Assert-True ($run.Code -ne 0 -and $run.Output -match $case.Expect) "$($case.Name) - exit $($run.Code), output: $($run.Output)"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $dir 'winget'))) "$($case.Name) - wrote manifests anyway"
    }
    $empty = Join-Path $root 'empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    $run = Invoke-Manifest @('-Version', '1.2.3', '-ReleaseDir', $empty)
    Assert-True ($run.Code -ne 0 -and $run.Output -match 'SHA256SUMS not found') "No SHA256SUMS: $($run.Code) $($run.Output)"

    Write-Host 'winget-manifest.ps1 tests passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
