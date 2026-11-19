# Tests for tools/check-winget-manifests.ps1, which release.yml's dry run runs on the previous release's WinGet
# manifests (#3822): schema, version, installer URLs and hashes. The manifests are the ones
# tools/winget-manifest.ps1 writes from fixture installers, then broken one way at a time. Registered in CTest on
# Windows with PowerShell 7; can also run directly with pwsh -File. Nothing is downloaded or installed.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'check-winget-manifests.ps1'), [ref]$null, [ref]$null)
$definitions = @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false))
Assert-True (@($definitions | Where-Object Name -eq 'Test-WingetManifests').Count -eq 1) 'check-winget-manifests.ps1 has no Test-WingetManifests function'
$PackageId = 'TaskSmack.TaskSmack'
# The checker's YAML reader and its helpers come along with Test-WingetManifests.
. ([scriptblock]::Create(($definitions | ForEach-Object { $_.Extent.Text }) -join "`n"))

$releaseYml = Join-Path (Split-Path -Parent $PSScriptRoot) '.github/workflows/release.yml'
$pinned = @(Get-Content -LiteralPath $releaseYml | Where-Object { $_ -match "^\s+WINGETCREATE_MANIFEST_VERSION:\s*'([0-9.]+)'" } | ForEach-Object { $Matches[1] })
Assert-True ($pinned.Count -eq 1) "release.yml sets WINGETCREATE_MANIFEST_VERSION $($pinned.Count) times"
$manifestVersion = $pinned[0]
$pwsh = (Get-Process -Id $PID).Path
$repository = 'mgradwohl/tasksmack'

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-check-winget-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $installers = Join-Path $root 'installers'
    New-Item -ItemType Directory -Path $installers | Out-Null
    $sums = foreach ($name in @('TaskSmack-1.2.3-win64.exe', 'TaskSmack-1.2.3-windows-arm64.exe')) {
        [IO.File]::WriteAllText((Join-Path $installers $name), "fixture $name")
        "$((Get-FileHash -LiteralPath (Join-Path $installers $name) -Algorithm SHA256).Hash.ToLowerInvariant())  $name"
    }
    [IO.File]::WriteAllText((Join-Path $installers 'SHA256SUMS'), (($sums -join "`n") + "`n"))
    & $pwsh -NoProfile -File (Join-Path $PSScriptRoot 'winget-manifest.ps1') -Version 1.2.3 -ReleaseDir $installers -ReleaseDate 2026-12-01 -OutDir (Join-Path $root 'out') | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) "winget-manifest.ps1 failed ($LASTEXITCODE)"
    $good = Join-Path $root 'out/manifests/t/TaskSmack/TaskSmack/1.2.3'

    function Test-Copy([scriptblock]$Break) {
        $copy = Join-Path $root "copy-$([guid]::NewGuid().ToString('N'))/1.2.3"
        New-Item -ItemType Directory -Path (Split-Path -Parent $copy) | Out-Null
        Copy-Item -LiteralPath $good -Destination $copy -Recurse
        & $Break $copy
        return , @(Test-WingetManifests $copy '1.2.3' 'v1.2.3' $repository $manifestVersion $installers)
    }
    function Edit-File([string]$Dir, [string]$Name, [string]$Pattern, [string]$Replacement) {
        $path = Join-Path $Dir $Name
        $text = [IO.File]::ReadAllText($path)
        Assert-True ($text -match $Pattern) "$Name has no text matching $Pattern"
        [IO.File]::WriteAllText($path, [regex]::Replace($text, $Pattern, $Replacement))
    }
    function Has-Problem([string[]]$Problems, [string]$Pattern) { return @($Problems | Where-Object { $_ -match $Pattern }).Count -gt 0 }

    # What winget-manifest.ps1 writes passes.
    $problems = @(Test-WingetManifests $good '1.2.3' 'v1.2.3' $repository $manifestVersion $installers)
    Assert-True ($problems.Count -eq 0) "The generated manifests were refused: $($problems -join '; ')"
    # A hash in lower case still passes.
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' 'InstallerSha256: ([0-9A-F]{64})' ('InstallerSha256: ' + ('a' * 64)) }
    Assert-True (Has-Problem $problems 'InstallerSha256 is a{64}, but') 'A lower-case hash that is wrong wasn''t compared'
    $lower = Join-Path $root 'lower/1.2.3'
    New-Item -ItemType Directory -Path (Split-Path -Parent $lower) | Out-Null
    Copy-Item -LiteralPath $good -Destination $lower -Recurse
    $installerManifest = Join-Path $lower 'TaskSmack.TaskSmack.installer.yaml'
    [IO.File]::WriteAllText($installerManifest, [regex]::Replace([IO.File]::ReadAllText($installerManifest), 'InstallerSha256: (\w+)', { param($m) 'InstallerSha256: ' + $m.Groups[1].Value.ToLowerInvariant() }))
    $problems = @(Test-WingetManifests $lower '1.2.3' 'v1.2.3' $repository $manifestVersion $installers)
    Assert-True ($problems.Count -eq 0) "A lower-case hash was refused: $($problems -join '; ')"

    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' 'ManifestVersion: \S+' 'ManifestVersion: 1.4.0' }
    Assert-True (Has-Problem $problems 'installer.yaml is on ManifestVersion \[1\.4\.0\]') "A wrong ManifestVersion wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.yaml' 'winget-manifest\.version\.\S+\.schema' 'winget-manifest.version.1.4.0.schema' }
    Assert-True (Has-Problem $problems 'schema header is \[1\.4\.0\]') "A wrong schema header wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.locale.en-US.yaml' 'PackageVersion: \S+' 'PackageVersion: 1.2.4' }
    Assert-True (Has-Problem $problems 'locale\.en-US\.yaml''s PackageVersion is \[1\.2\.4\], not 1\.2\.3') "A wrong PackageVersion wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.yaml' 'PackageIdentifier: \S+' 'PackageIdentifier: Other.Package' }
    Assert-True (Has-Problem $problems 'PackageIdentifier') "A wrong PackageIdentifier wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' '/v1\.2\.3/TaskSmack-1\.2\.3-win64' '/v1.2.2/TaskSmack-1.2.3-win64' }
    Assert-True (Has-Problem $problems 'x64 InstallerUrl') "A wrong URL wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' '(?<=arm64\s+InstallerUrl: \S+\s+InstallerSha256: )[0-9A-F]{64}' ('0' * 64) }
    Assert-True (Has-Problem $problems 'arm64 InstallerSha256 is 0{64}, but TaskSmack-1\.2\.3-windows-arm64\.exe hashes to') "A wrong hash wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' '(?s)  - Architecture: arm64.*?(?=ManifestType)' '' }
    Assert-True (Has-Problem $problems 'lists no arm64 installer') "A missing installer wasn't named: $($problems -join '; ')"
    # #4436: the installer's exit code for a running TaskSmack, 32, must be WinGet's packageInUse for both installers.
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' '(?m)^ExpectedReturnCodes:\r?\n(  .*\r?\n)+' '' }
    Assert-True ((Has-Problem $problems 'x64 installer''s ExpectedReturnCodes don''t map InstallerReturnCode 32') -and (Has-Problem $problems 'arm64 installer''s ExpectedReturnCodes')) "Missing ExpectedReturnCodes weren't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' 'ReturnResponse: packageInUse' 'ReturnResponse: installInProgress' }
    Assert-True (Has-Problem $problems 'x64 installer''s ExpectedReturnCodes') "installInProgress for 32 wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' 'InstallerReturnCode: 32' 'InstallerReturnCode: 2' }
    Assert-True (Has-Problem $problems 'arm64 installer''s ExpectedReturnCodes') "Another exit code wasn't named: $($problems -join '; ')"
    # An installer's own ExpectedReturnCodes replace the root's, so one without the code is named.
    $problems = Test-Copy { param($d) Edit-File $d 'TaskSmack.TaskSmack.installer.yaml' '(?m)^(  - Architecture: arm64\r?\n)' "`$1    ExpectedReturnCodes:`n      - InstallerReturnCode: 1`n        ReturnResponse: cancelledByUser`n" }
    Assert-True ((Has-Problem $problems 'arm64 installer''s ExpectedReturnCodes') -and -not (Has-Problem $problems 'x64 installer''s ExpectedReturnCodes')) "An installer's own ExpectedReturnCodes without 32 wasn't named: $($problems -join '; ')"
    $problems = Test-Copy { param($d) Remove-Item -LiteralPath (Join-Path $d 'TaskSmack.TaskSmack.locale.en-US.yaml') }
    Assert-True (Has-Problem $problems 'expected TaskSmack\.TaskSmack\.yaml') "A missing locale manifest wasn't named: $($problems -join '; ')"
    $problems = @(Test-WingetManifests $good '1.2.4' 'v1.2.4' $repository $manifestVersion $installers)
    Assert-True ((Has-Problem $problems 'the folder is named 1\.2\.3, not 1\.2\.4') -and (Has-Problem $problems 'PackageVersion')) "A folder of another version wasn't refused: $($problems -join '; ')"
    $problems = @(Test-WingetManifests (Join-Path $root 'missing') '1.2.3' 'v1.2.3' $repository $manifestVersion $installers)
    Assert-True (Has-Problem $problems "isn't a folder") "A missing folder wasn't named: $($problems -join '; ')"

    # The layouts wingetcreate writes (#4437): "Installers:" with its "- Architecture:" items at column 0 (its serializer
    # has no indented sequences), fields shifted to the root or left on each installer, quoted and unquoted scalars,
    # comments, CRLF. The version and locale manifests stay the generated ones.
    $fixtureDir = Join-Path $PSScriptRoot 'test-fixtures/winget'
    $sumX64 = (Get-FileHash -LiteralPath (Join-Path $installers 'TaskSmack-1.2.3-win64.exe') -Algorithm SHA256).Hash
    $sumArm64 = (Get-FileHash -LiteralPath (Join-Path $installers 'TaskSmack-1.2.3-windows-arm64.exe') -Algorithm SHA256).Hash
    function Test-Fixture([string]$Name, [string]$Eol, [scriptblock]$Mutate) {
        return Test-Copy {
            param($d)
            $text = [IO.File]::ReadAllText((Join-Path $fixtureDir "$Name.installer.yaml")).Replace('__VERSION__', '1.2.3').Replace('__SHA_X64__', $sumX64).Replace('__SHA_ARM64__', $sumArm64)
            if ($Mutate) { $text = & $Mutate $text }
            [IO.File]::WriteAllText((Join-Path $d 'TaskSmack.TaskSmack.installer.yaml'), $text.Replace("`r`n", "`n").Replace("`n", $Eol))
        }
    }
    foreach ($name in @('wingetcreate-root-level', 'wingetcreate-per-installer')) {
        foreach ($eol in @("`n", "`r`n")) {
            $label = "$name ($(if ($eol -eq "`n") { 'LF' } else { 'CRLF' }))"
            $problems = Test-Fixture $name $eol $null
            Assert-True ($problems.Count -eq 0) "The $label fixture was refused: $($problems -join '; ')"
            $problems = Test-Fixture $name $eol { param($t) $t.Replace("v1.2.3/TaskSmack-1.2.3-win64", "v1.2.2/TaskSmack-1.2.3-win64") }
            Assert-True (Has-Problem $problems 'x64 InstallerUrl') "$label, a wrong URL wasn't named: $($problems -join '; ')"
            $problems = Test-Fixture $name $eol { param($t) $t.Replace($sumArm64, ('0' * 64)) }
            Assert-True (Has-Problem $problems 'arm64 InstallerSha256 is 0{64}, but') "$label, a wrong hash wasn't named: $($problems -join '; ')"
            $problems = Test-Fixture $name $eol { param($t) $t.Replace('Architecture: arm64', 'Architecture: x64') }
            Assert-True ((Has-Problem $problems 'x64 installer is listed twice') -and (Has-Problem $problems 'lists no arm64 installer')) "$label, a repeated architecture wasn't named: $($problems -join '; ')"
            $problems = Test-Fixture $name $eol { param($t) $t.Replace('ReturnResponse: packageInUse', 'ReturnResponse: fileInUse') }
            Assert-True ((Has-Problem $problems 'x64 installer''s ExpectedReturnCodes') -and (Has-Problem $problems 'arm64 installer''s ExpectedReturnCodes')) "$label, a wrong ReturnResponse for 32 wasn't named: $($problems -join '; ')"
            $problems = Test-Fixture $name $eol { param($t) $t.Replace('ManifestVersion: 1.12.0', 'ManifestVersion: 1.4.0') }
            Assert-True (Has-Problem $problems 'installer\.yaml is on ManifestVersion \[1\.4\.0\]') "$label, a wrong ManifestVersion wasn't named: $($problems -join '; ')"
        }
    }
    # A manifest outside the YAML subset is named, never misread as having no installers.
    $problems = Test-Fixture 'wingetcreate-root-level' "`n" { param($t) $t.Replace('Installers:', "Installers: &anchor`n- Architecture: x64`n  Architecture: x64`nOther:") }
    Assert-True (Has-Problem $problems 'installer\.yaml can''t be read as YAML') "An unreadable manifest wasn't named: $($problems -join '; ')"

    Write-Host 'check-winget-manifests tests passed'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
