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
        Assert-True ($text -match '(?m)^ManifestVersion: 1\.28\.0$') "${name}: no ManifestVersion 1.28.0"
        # Only the schema comment is kept; the templates' own notes stay in packaging/winget/.
        $comments = @($text -split "`n" | Where-Object { $_ -match '^\s*#' })
        Assert-True ($comments.Count -eq 1 -and $comments[0] -like '# yaml-language-server: $schema=https://aka.ms/winget-manifest.*.1.28.0.schema.json') "$name comments: $($comments -join ' | ')"
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
