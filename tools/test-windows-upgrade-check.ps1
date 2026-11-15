# Tests the decisions of release.yml's Windows upgrade check (#3757): which release
# tools/fetch-previous-windows-release.ps1 upgrades from, and how it checks the download against SHA256SUMS,
# and how tools/windows-upgrade-check.ps1 judges the installed folder, the winget manifest's entry and the
# saved config.toml. Only those functions run, loaded from the scripts' ASTs: the scripts themselves
# download releases and run installers, so they never run here. Nothing is downloaded or installed.
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

# The named functions' definitions in a script, to dot-source here (a function's own dot-sourcing would
# define them only in its scope).
function Get-ScriptFunctions([string]$Script, [string[]]$Names) {
    $ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $Script), [ref]$null, [ref]$null)
    foreach ($name in $Names) {
        $functionAst = $ast.Find({
                param($node)
                $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
            }, $true)
        Assert-True ($null -ne $functionAst) "$Script has no $name function"
        [scriptblock]::Create($functionAst.Extent.Text)
    }
}

foreach ($definition in @(Get-ScriptFunctions 'fetch-previous-windows-release.ps1' @('ConvertTo-ReleaseVersion', 'Select-PreviousRelease',
        'Get-WindowsInstallerNames', 'Test-AgainstSha256Sums', 'Get-CosignIdentity')) +
    @(Get-ScriptFunctions 'windows-upgrade-check.ps1' @('Get-InstallerVersion', 'Get-FileTree', 'Compare-InstalledTree',
            'Get-WingetEntry', 'Get-TomlValue', 'Test-FixtureConfig', 'Get-FixtureSettings'))) {
    . $definition
}

function New-Release([string]$Tag, [string[]]$Assets = @(), [switch]$Draft, [switch]$Prerelease) {
    return [pscustomobject]@{ tag_name = $Tag; draft = [bool]$Draft; prerelease = [bool]$Prerelease; assets = $Assets }
}

# The release to upgrade from: the highest published vX.Y.Z below the build's version.
$releases = @(
    (New-Release 'v0.11.0'), (New-Release 'v0.10.1'), (New-Release 'v0.10.0'), (New-Release 'v0.9.1'),
    (New-Release 'v0.10.2' -Draft), (New-Release 'v0.10.3' -Prerelease), (New-Release 'v0.10.9-rc1'),
    (New-Release 'v0.010.5'), (New-Release 'nightly')
)
$pick = { param($version) $r = Select-PreviousRelease $releases ([version]$version); if ($r) { $r.tag_name } else { '' } }
Assert-True ((& $pick '0.12.0') -eq 'v0.11.0') "A 0.12.0 build should upgrade from v0.11.0, not $(& $pick '0.12.0')"
Assert-True ((& $pick '0.11.0') -eq 'v0.10.1') "A 0.11.0 build should upgrade from v0.10.1 (never itself), not $(& $pick '0.11.0')"
# A patch on release/0.10 after v0.11.0 shipped upgrades from the line's last patch, never down from v0.11.0;
# drafts, pre-releases and tags that aren't strict vX.Y.Z don't count.
Assert-True ((& $pick '0.10.4') -eq 'v0.10.1') "A 0.10.4 build should upgrade from v0.10.1, not $(& $pick '0.10.4')"
Assert-True ((& $pick '0.10.0') -eq 'v0.9.1') "A 0.10.0 build should upgrade from v0.9.1, not $(& $pick '0.10.0')"
Assert-True ((& $pick '0.9.1') -eq '') "A 0.9.1 build has nothing below it here, but got $(& $pick '0.9.1')"
Assert-True ($null -eq (Select-PreviousRelease @() ([version]'1.0.0'))) 'No releases should pick none'
Assert-True ($null -eq (ConvertTo-ReleaseVersion 'v1.2')) 'v1.2 is not a release tag'
Assert-True ((ConvertTo-ReleaseVersion 'v0.10.0') -eq [version]'0.10.0') 'v0.10.0 should parse as 0.10.0'

# The installers it ships, by the names release.yml gives them.
$names = Get-WindowsInstallerNames @('TaskSmack-0.10.0-win64.exe', 'TaskSmack-0.10.0-windows-arm64.exe', 'TaskSmack-0.10.0-win64.zip', 'SHA256SUMS') '0.10.0'
Assert-True (($names -join ',') -eq 'TaskSmack-0.10.0-win64.exe,TaskSmack-0.10.0-windows-arm64.exe') "Expected both installers, got $($names -join ',')"
$names = Get-WindowsInstallerNames @('TaskSmack-0.8.0-win64.zip', 'TaskSmack-0.8.0-Linux.deb') '0.8.0'
Assert-True ($names.Count -eq 0) "A release with no installer has none, got $($names -join ',')"
$names = Get-WindowsInstallerNames @('TaskSmack-0.9.0-win64.exe') '0.10.0'
Assert-True ($names.Count -eq 0) "Another version's installer isn't this release's, got $($names -join ',')"

# The cosign identity: this repo's release.yml at the release's own tag, and nothing else.
$identity = Get-CosignIdentity 'mgradwohl/tasksmack' 'v0.10.0'
Assert-True ('https://github.com/mgradwohl/tasksmack/.github/workflows/release.yml@refs/tags/v0.10.0' -match $identity) "The identity $identity doesn't match the release's own certificate"
foreach ($other in 'https://github.com/mgradwohl/tasksmack/.github/workflows/release.yml@refs/tags/v0.10.01',
    'https://github.com/mgradwohl/tasksmack/.github/workflows/release.yml@refs/heads/main',
    'https://github.com/mgradwohl/tasksmack/.github/workflows/ci.yml@refs/tags/v0.10.0',
    'https://github.com/someone/tasksmack/.github/workflows/release.yml@refs/tags/v0.10.0',
    'https://github.com/mgradwohl/tasksmackX.github/workflows/release.yml@refs/tags/v0x10x0') {
    Assert-True ($other -notmatch $identity) "The identity $identity also matches $other"
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-upgrade-check-test-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    # SHA256SUMS: every installer listed once, with its SHA-256.
    $download = Join-Path $scratch 'download'
    New-Item -ItemType Directory -Path $download | Out-Null
    Set-Content -LiteralPath (Join-Path $download 'a.exe') -Value 'installer a' -NoNewline
    Set-Content -LiteralPath (Join-Path $download 'b.exe') -Value 'installer b' -NoNewline
    $hashA = (Get-FileHash -LiteralPath (Join-Path $download 'a.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashB = (Get-FileHash -LiteralPath (Join-Path $download 'b.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    $sums = Join-Path $download 'SHA256SUMS'
    Set-Content -LiteralPath $sums -Value "$hashA  a.exe`n$($hashB.ToUpperInvariant())  b.exe`n$('0' * 64)  other.zip`n"
    $problems = Test-AgainstSha256Sums $download @('a.exe', 'b.exe')
    Assert-True ($problems.Count -eq 0) "Matching installers were refused: $($problems -join '; ')"
    Set-Content -LiteralPath $sums -Value "$hashA  a.exe`n$hashA  b.exe`n"
    $problems = Test-AgainstSha256Sums $download @('a.exe', 'b.exe')
    Assert-True ($problems.Count -eq 1 -and $problems[0] -match '^b\.exe has SHA-256') "A wrong SHA-256 wasn't caught: $($problems -join '; ')"
    Set-Content -LiteralPath $sums -Value "$hashA  a.exe`n"
    $problems = Test-AgainstSha256Sums $download @('a.exe', 'b.exe')
    Assert-True ($problems.Count -eq 1 -and $problems[0] -match 'no line for b\.exe') "A missing line wasn't caught: $($problems -join '; ')"
    Set-Content -LiteralPath $sums -Value "$hashA  a.exe`n$hashA  a.exe`nnot a line`n"
    $problems = Test-AgainstSha256Sums $download @('a.exe')
    Assert-True ($problems.Count -eq 2) "A repeated name and a malformed line weren't both caught: $($problems -join '; ')"

    # The installed folder after the upgrade, against the new installer's files and the previous version's.
    $expected = @{ 'TaskSmack.exe' = 'NEW1'; 'share\a.toml' = 'NEW2'; 'Uninstall.exe' = 'UNINST' }
    $previous = @{ 'TaskSmack.exe' = 'OLD1'; 'share\a.toml' = 'NEW2'; 'Uninstall.exe' = 'OLDUN'; 'old.dll' = 'OLD3' }
    Assert-True ((Compare-InstalledTree $expected $expected.Clone() $previous).Count -eq 0) 'An exact install was refused'
    $installed = $expected.Clone(); $installed['TaskSmack.exe'] = 'OLD1'; $installed['old.dll'] = 'OLD3'; $installed.Remove('share\a.toml')
    $problems = Compare-InstalledTree $expected $installed $previous
    Assert-True ($problems.Count -eq 3) "Expected three problems, got: $($problems -join '; ')"
    Assert-True (@($problems -match "TaskSmack\.exe differs .*still the previous version's").Count -eq 1) "An exe that wasn't replaced wasn't named: $($problems -join '; ')"
    Assert-True (@($problems -match '^old\.dll, from the previous version, is still installed').Count -eq 1) "A stale file wasn't named: $($problems -join '; ')"
    Assert-True (@($problems -match "^share\\a\.toml isn't installed").Count -eq 1) "A missing file wasn't named: $($problems -join '; ')"
    $installed = $expected.Clone(); $installed['extra.txt'] = 'X'; $installed['Uninstall.exe'] = 'OTHER'
    $problems = Compare-InstalledTree $expected $installed $previous
    Assert-True ($problems.Count -eq 2 -and ($problems -match "Uninstall\.exe differs .*isn't the new installer's") -and ($problems -match "^extra\.txt is installed but the new installer doesn't ship it")) "An unknown file and a wrong uninstaller weren't both caught: $($problems -join '; ')"

    # Get-FileTree: relative paths, nested, hidden files included; a missing folder is empty.
    $tree = Join-Path $scratch 'tree'
    New-Item -ItemType Directory -Path (Join-Path $tree 'share\themes') | Out-Null
    Set-Content -LiteralPath (Join-Path $tree 'TaskSmack.exe') -Value 'exe' -NoNewline
    Set-Content -LiteralPath (Join-Path $tree 'share\themes\x.toml') -Value 'x' -NoNewline
    $hidden = New-Item -ItemType File -Path (Join-Path $tree '.hidden')
    $hidden.Attributes = $hidden.Attributes -bor [IO.FileAttributes]::Hidden
    $files = Get-FileTree $tree
    Assert-True ((($files.Keys | Sort-Object) -join ',') -eq '.hidden,share\themes\x.toml,TaskSmack.exe') "Get-FileTree listed $(($files.Keys | Sort-Object) -join ',')"
    Assert-True ((Get-FileTree (Join-Path $scratch 'missing')).Count -eq 0) 'A missing folder should list no files'

    Write-Host 'Checked the release choice, SHA256SUMS, cosign identity and installed-folder comparison.'
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# Installer names.
Assert-True ((Get-InstallerVersion 'C:\x\TaskSmack-0.10.0-win64.exe') -eq '0.10.0') 'The x64 installer name should give 0.10.0'
Assert-True ((Get-InstallerVersion 'TaskSmack-0.11.0-windows-arm64.exe') -eq '0.11.0') 'The ARM64 installer name should give 0.11.0'
Assert-True ($null -eq (Get-InstallerVersion 'TaskSmack-0.11.0-win64.zip')) 'A .zip is no installer'

# The winget manifest's AppsAndFeaturesEntries, from the real template.
$manifest = Join-Path $PSScriptRoot '..\packaging\winget\TaskSmack.TaskSmack.installer.yaml'
$entry = Get-WingetEntry (Get-Content -LiteralPath $manifest -Raw)
Assert-True ($entry['DisplayName'] -ceq 'TaskSmack' -and $entry['Publisher'] -ceq 'TaskSmack Contributors' -and $entry['ProductCode'] -ceq 'TaskSmack') `
    "The winget manifest's entry read as $(($entry.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')"
$entry = Get-WingetEntry "ProductCode: Outer`nAppsAndFeaturesEntries:`n  - DisplayName: 'A'`n    Publisher: `"B`"`n    ProductCode: C`n  - DisplayName: Second`nManifestType: installer"
Assert-True ($entry['DisplayName'] -ceq 'A' -and $entry['Publisher'] -ceq 'B' -and $entry['ProductCode'] -ceq 'C') "Only the first entry, unquoted, should be read: $(($entry.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')"

# The saved config.toml: v0.8.0's own file has the theme but no config_version yet (its font size is a key a
# later version renamed); a saved one with its settings passes,
# and a lost setting is named.
$settings = @(@('theme', 'id', "'dracula'"), @('font', 'point_size', '10'))
$fixture = @(Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\tests\App\fixtures\config-v0.8.0.toml'))
$problems = Test-FixtureConfig $fixture @(,$settings[0])
Assert-True ($problems.Count -eq 1 -and $problems[0] -match 'no config_version') "v0.8.0's own file should only lack config_version: $($problems -join '; ')"
$saved = @('config_version = 3', '', '[theme]', "id = 'dracula'", '', '[font]', 'point_size = 10')
Assert-True ((Test-FixtureConfig $saved $settings).Count -eq 0) 'A saved config with the settings kept was refused'
$saved[6] = 'point_size = 12'
$problems = Test-FixtureConfig $saved $settings
Assert-True ($problems.Count -eq 1 -and $problems[0] -match "^\[font\] point_size is 12, not v0\.8\.0's 10$") "A lost setting wasn't named: $($problems -join '; ')"

# The previous release's own save is held to the keys unchanged since v0.8.0 (#3757: v0.10.0 writes
# [font] size = 'large', not point_size); the upgraded build's save is held to all of them.
$full = Get-FixtureSettings
$stable = Get-FixtureSettings -Stable
Assert-True ($full.Count -eq 7 -and @($full | Where-Object { $_[0] -eq 'font' -and $_[1] -eq 'point_size' }).Count -eq 1) "The full list should have 7 settings with [font] point_size, got $($full.Count)"
Assert-True ($stable.Count -eq 6 -and @($stable | Where-Object { $_[0] -eq 'font' }).Count -eq 0) "The stable list should be the other 6, got $(($stable | ForEach-Object { "$($_[0]).$($_[1])" }) -join ', ')"
$v0100 = @('config_version = 2', '', '[font]', "size = 'large'", '', '[process_columns]', 'threads = true', 'user = false', '',
    '[sampling]', 'history_max_seconds = 600', 'interval_ms = 2000', '', '[theme]', "id = 'dracula'", '', '[ui]', 'show_privilege_notice = false')
$problems = Test-FixtureConfig $v0100 $stable
Assert-True ($problems.Count -eq 0) "v0.10.0's save should pass the stable list: $($problems -join '; ')"
$problems = Test-FixtureConfig $v0100 $full
Assert-True ($problems.Count -eq 1 -and $problems[0] -match '^\[font\] point_size is missing') "v0.10.0's save should fail the full list on point_size alone: $($problems -join '; ')"
Write-Host 'windows-upgrade-check tests passed'
