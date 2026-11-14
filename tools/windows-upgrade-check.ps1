#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Install this run's Windows installer over the last published release's, then check the upgrade and the
    uninstall (#3757).
.DESCRIPTION
    release.yml's validate-windows-upgrade job, once per architecture, on a GitHub-hosted runner that is
    thrown away afterwards: it installs into Program Files, writes the registry and runs the installers, so
    it never runs anywhere else. tools/fetch-previous-windows-release.ps1 picked, downloaded and verified
    the previous installer (Sigstore-signed SHA256SUMS); the job checked its Authenticode signature.
    validate-windows-package's round trip upgrades this run's installer over itself; this one upgrades over
    the release users have, as validate-linux-upgrade does with v0.8.0's .deb.

    1. The previous installer, with /S. Then exactly one Programs and Features entry (the Uninstall key,
       in any of the three views), with the previous version; TaskSmack.exe in Program Files\TaskSmack.
       Recorded: the version, the install folder and its files, the Uninstall key's values, App Paths.
    2. The user's config: the config.toml v0.8.0 saved (tests/App/fixtures/config-v0.8.0.toml) goes in
       %APPDATA%\TaskSmack, and the installed previous TaskSmack.exe runs once from it, headless, quitting
       by itself (TASKSMACK_SCREENSHOT) and saving it in its own format. Start at sign-in is turned on
       for that copy as TaskSmack writes it (HKCU Run value TaskSmack: "<exe>" --minimized), and a
       sentinel value goes into the Uninstall key, which only the old uninstaller deletes.
    3. This run's installer, with /S, over it.
    4. Checked: one Programs and Features entry, with the new version, the sentinel gone (the old
       uninstaller ran), its DisplayName, Publisher and key name as the winget manifest's
       AppsAndFeaturesEntries give them, and its UninstallString the installed Uninstall.exe; the
       install folder holds exactly the new .zip's files, byte for byte, plus the expected Uninstall.exe,
       so every file was replaced and no stale one is left; TaskSmack.exe's ProductVersion is the new
       version; %APPDATA%\TaskSmack unchanged; the Run value still names the installed TaskSmack.exe;
       the Start menu shortcut targets it; no uninstall mark; App Paths, if there was one, names it.
    5. The upgraded TaskSmack.exe starts headless from the user's config: it must quit by itself with exit
       code 0, a frame written, no [critical] or [error] log line, and config.toml saved with v0.8.0's
       settings kept.
    6. Its Uninstall.exe, with /S. Then the install folder, the Uninstall key, the app's own registry key,
       App Paths, the Start menu entries and the Run value are gone, and the uninstall mark is written.

    Mesa's llvmpipe DLLs go beside the installed exe for each launch only, and are deleted after it, so
    the install folder holds only what an installer put there. Appends a summary to
    $env:GITHUB_STEP_SUMMARY when it is set, and exits 1 on any failure.
.PARAMETER PreviousInstaller
    The previous release's installer (TaskSmack-<version>-win64.exe or -windows-arm64.exe).
.PARAMETER Installer
    This run's installer, for the same architecture.
.PARAMETER PackageRoot
    This run's .zip, extracted: its top-level folder, which holds TaskSmack.exe. The installer installs
    exactly these files.
.PARAMETER ExpectedUninstaller
    The Uninstall.exe this run's installer installs: sign-windows' signed one, or build-windows' own.
.PARAMETER MesaDllDir
    The folder holding Mesa's llvmpipe DLLs (.github/actions/setup-mesa-llvmpipe's dll-dir).
.PARAMETER Fixture
    The config.toml v0.8.0 saved: tests/App/fixtures/config-v0.8.0.toml.
.PARAMETER WingetManifest
    packaging/winget/TaskSmack.TaskSmack.installer.yaml, whose AppsAndFeaturesEntries the entry must match.
.PARAMETER Title
    The heading of the step summary.
#>
param(
    [Parameter(Mandatory)][string]$PreviousInstaller,
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$ExpectedUninstaller,
    [Parameter(Mandatory)][string]$MesaDllDir,
    [Parameter(Mandatory)][string]$Fixture,
    [Parameter(Mandatory)][string]$WingetManifest,
    [string]$Title = "Windows upgrade over the previous release ($env:RUNNER_ARCH)"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$UninstallRoots = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
)
$AppPathsKeys = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\TaskSmack.exe',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\TaskSmack.exe',
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\TaskSmack.exe'
)
$AppKeys = @('HKLM:\SOFTWARE\TaskSmack Contributors\TaskSmack', 'HKLM:\SOFTWARE\WOW6432Node\TaskSmack Contributors\TaskSmack')
$RunKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
$SentinelName = 'TaskSmackCiUpgradeSentinel'
# The settings v0.8.0's config.toml changed from the defaults: section, key, and the value as TaskSmack
# writes it. Keep in step with tools/release-smoke-check.ps1's list.
$FixtureSettings = @(
    @('theme', 'id', "'dracula'"),
    @('font', 'point_size', '10'),
    @('sampling', 'interval_ms', '2000'),
    @('sampling', 'history_max_seconds', '600'),
    @('ui', 'show_privilege_notice', 'false'),
    @('process_columns', 'threads', 'true'),
    @('process_columns', 'user', 'false')
)

# The version in an installer's name: TaskSmack-<version>-win64.exe or -windows-arm64.exe, else $null.
function Get-InstallerVersion([string]$Path) {
    if ((Split-Path -Leaf $Path) -cmatch '^TaskSmack-(.+)-(win64|windows-arm64)\.exe$') { return $Matches[1] }
    return $null
}

# Every file under $Root, by its path relative to $Root, with its SHA-256; @{} when $Root doesn't exist.
function Get-FileTree([string]$Root) {
    $tree = @{}
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return $tree }
    $full = (Resolve-Path -LiteralPath $Root).ProviderPath.TrimEnd('\')
    Get-ChildItem -LiteralPath $full -Recurse -File -Force | ForEach-Object {
        $tree[$_.FullName.Substring($full.Length).TrimStart('\')] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
    return $tree
}

# The problems with an installed folder ($Installed, a Get-FileTree) after an upgrade, against what this
# run's installer installs ($Expected): a file missing, or not byte for byte the expected one, and a file
# that isn't expected, named as stale when the previous version installed it ($Previous). Empty when the
# folder is exactly $Expected.
function Compare-InstalledTree([hashtable]$Expected, [hashtable]$Installed, [hashtable]$Previous) {
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @($Expected.Keys | Sort-Object)) {
        if (-not $Installed.ContainsKey($name)) { $problems.Add("$name isn't installed") }
        elseif ($Installed[$name] -ne $Expected[$name]) {
            $how = if ($Previous.ContainsKey($name) -and $Previous[$name] -eq $Installed[$name]) { "it's still the previous version's" } else { "it isn't the new installer's" }
            $problems.Add("the installed $name differs from the expected one: $how")
        }
    }
    foreach ($name in @($Installed.Keys | Sort-Object)) {
        if ($Expected.ContainsKey($name)) { continue }
        if ($Previous.ContainsKey($name)) { $problems.Add("$name, from the previous version, is still installed though the new one doesn't ship it") }
        else { $problems.Add("$name is installed but the new installer doesn't ship it") }
    }
    return , $problems.ToArray()
}

# The winget installer manifest's AppsAndFeaturesEntries: the first entry's DisplayName, Publisher and
# ProductCode, which identify an installed TaskSmack to winget.
function Get-WingetEntry([string]$Text) {
    $entry = @{}
    $inEntries = $false
    foreach ($line in $Text -split '\r?\n') {
        if ($line -match '^AppsAndFeaturesEntries:\s*$') { $inEntries = $true; continue }
        if (-not $inEntries) { continue }
        if ($line -match '^\S') { break }
        if ($line -match '^\s+(?:-\s+)?(DisplayName|Publisher|ProductCode):\s*(.+?)\s*$') {
            if ($entry.ContainsKey($Matches[1])) { break }
            $entry[$Matches[1]] = $Matches[2].Trim("'", '"')
        }
    }
    return $entry
}

# The value of <key> in [<section>] of a config.toml, as written.
function Get-TomlValue([string[]]$Lines, [string]$Section, [string]$Key) {
    $current = ''
    foreach ($line in $Lines) {
        if ($line -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
        if ($current -eq $Section -and $line -match '^(\S+) = (.*)$' -and $Matches[1] -eq $Key) { return $Matches[2] }
    }
    return $null
}

# The problems with a config.toml a TaskSmack saved from v0.8.0's: no config_version (it wasn't rewritten),
# or one of v0.8.0's settings lost.
function Test-FixtureConfig([string[]]$Lines, [object[]]$Settings) {
    $problems = [System.Collections.Generic.List[string]]::new()
    if (-not ($Lines -match '^config_version = \d+$')) { $problems.Add("it has no config_version, so it wasn't saved over v0.8.0's file") }
    foreach ($setting in $Settings) {
        $actual = Get-TomlValue $Lines $setting[0] $setting[1]
        if ($actual -cne $setting[2]) {
            $problems.Add("[$($setting[0])] $($setting[1]) is $(if ($null -eq $actual) { 'missing' } else { $actual }), not v0.8.0's $($setting[2])")
        }
    }
    return , $problems.ToArray()
}

# tools/test-windows-upgrade-check.ps1 loads the functions above from this script's AST; nothing below runs there.

# Every TaskSmack Programs and Features entry: an Uninstall key named TaskSmack or with TaskSmack in its
# DisplayName, in any of the three views.
function Get-UninstallEntries {
    foreach ($root in $UninstallRoots) {
        Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -match 'TaskSmack' -or "$($_.GetValue('DisplayName'))" -match 'TaskSmack' }
    }
}

function Get-StartMenuEntries {
    foreach ($root in "$env:ProgramData\Microsoft\Windows\Start Menu\Programs", "$env:APPDATA\Microsoft\Windows\Start Menu\Programs") {
        Get-ChildItem -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'TaskSmack' } | ForEach-Object FullName
    }
}

function Get-RunValue {
    $run = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($RunKeyPath)
    if (-not $run) { return $null }
    try { return $run.GetValue('TaskSmack', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) } finally { $run.Close() }
}

function Get-UninstallMark {
    $mark = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\TaskSmack')
    if (-not $mark) { return $null }
    try { return $mark.GetValue('Uninstalled') } finally { $mark.Close() }
}

function Get-AppPath {
    foreach ($key in $AppPathsKeys) {
        $item = Get-Item -LiteralPath $key -ErrorAction SilentlyContinue
        if ($item) { return [pscustomobject]@{ Key = $key; Default = $item.GetValue('') } }
    }
    return $null
}

# Runs an installer or uninstaller with /S and waits for it; throws unless it exits 0 in time.
function Invoke-Silently([string]$Path, [string]$What, [int]$TimeoutSeconds = 240) {
    $proc = Start-Process -FilePath $Path -ArgumentList '/S' -PassThru
    $null = $proc.Handle  # keep the handle, or ExitCode is empty once it has gone
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) { throw "$What didn't finish within $TimeoutSeconds s." }
    if ($proc.ExitCode -ne 0) { throw "$What exited with $($proc.ExitCode)." }
}

# Starts the installed TaskSmack.exe headless from the user's own config (%APPDATA%\TaskSmack: no
# TASKSMACK_CONFIG_DIR), on llvmpipe, to quit by itself after one frame. Returns its problems: a non-zero
# exit, no quit, no frame, and with -Strict any [critical] or [error] log line (only reported otherwise).
# The Mesa DLLs go beside the exe for the launch only.
function Invoke-HeadlessLaunch([string]$Exe, [string]$Label, [string]$LogDir, [switch]$Strict) {
    $problems = [System.Collections.Generic.List[string]]::new()
    $dir = Split-Path -Parent $Exe
    $dlls = @(Get-ChildItem -LiteralPath $MesaDllDir -Filter '*.dll' -File)
    $clashes = @($dlls | Where-Object { Test-Path -LiteralPath (Join-Path $dir $_.Name) })
    if ($clashes.Count) { $problems.Add("$dir already holds $(($clashes | ForEach-Object Name) -join ', '), which the Mesa DLLs would overwrite"); return , $problems.ToArray() }
    $shot = Join-Path $LogDir "$Label.png"
    $stdout = Join-Path $LogDir "$Label-stdout.log"
    $stderr = Join-Path $LogDir "$Label-stderr.log"
    $saved = @{}
    foreach ($name in 'GALLIUM_DRIVER', 'TASKSMACK_LOG_LEVEL', 'TASKSMACK_SCREENSHOT', 'TASKSMACK_CONFIG_DIR') { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
    try {
        $dlls | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dir }
        $env:GALLIUM_DRIVER = 'llvmpipe'
        $env:TASKSMACK_LOG_LEVEL = 'info'
        $env:TASKSMACK_SCREENSHOT = "$shot;after=5"
        $env:TASKSMACK_CONFIG_DIR = $null
        $proc = Start-Process -FilePath $Exe -WorkingDirectory $dir -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $proc.Handle
        if ($proc.WaitForExit(120000)) {
            if ($proc.ExitCode -ne 0) { $problems.Add("it exited with code $($proc.ExitCode); a clean quit after its frame exits 0") }
        } else {
            $problems.Add("it didn't quit by itself within 120 s")
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            $proc.WaitForExit(10000) | Out-Null
        }
        $lines = @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue) + @(Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue)
        Write-Host "=== $Label log ==="
        $lines | ForEach-Object { Write-Host $_ }
        Write-Host '=== end of log ==='
        $errors = @($lines | Where-Object { $_ -match '\[(critical|error)\]' })
        if ($errors.Count) {
            if ($Strict) { $problems.Add("its log has $($errors.Count) [critical]/[error] line(s):`n$($errors -join "`n")") }
            else { Write-Host "::warning::$Label's log has $($errors.Count) [critical]/[error] line(s); not this run's code, so only reported." }
        }
        if (-not $problems.Count -and -not (Test-Path -LiteralPath $shot -PathType Leaf)) { $problems.Add("it wrote no frame at $shot") }
    } finally {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
        foreach ($dll in $dlls) {
            try { Remove-Item -LiteralPath (Join-Path $dir $dll.Name) -Force -ErrorAction Stop }
            catch { $problems.Add("the Mesa DLL $($dll.Name) beside it didn't delete after it quit: $($_.Exception.Message)") }
        }
    }
    return , $problems.ToArray()
}

function Get-ShortcutTarget([string]$Path) {
    $shell = New-Object -ComObject WScript.Shell
    try { return $shell.CreateShortcut($Path).TargetPath } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
}

$summary = [System.Collections.Generic.List[string]]::new()
$failure = $null
function Add-Result([string]$Check, [string[]]$Problems, [string]$Passed) {
    if ($Problems.Count) {
        $Problems | ForEach-Object { Write-Host "::error::$Check`: $_" }
        $summary.Add("- $Check`: **FAILED** ($($Problems -join '; '))")
        $script:failure = "$Check`: $($Problems -join '; ')"
        throw $script:failure
    }
    Write-Host "$Check`: $Passed"
    $summary.Add("- $Check`: passed ($Passed)")
}

$previousVersion = Get-InstallerVersion $PreviousInstaller
$newVersion = Get-InstallerVersion $Installer
$installDir = Join-Path $env:ProgramW6432 'TaskSmack'
$exe = Join-Path $installDir 'TaskSmack.exe'
$configDir = Join-Path $env:APPDATA 'TaskSmack'
$logDir = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }) "tasksmack-upgrade-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $logDir | Out-Null

try {
    if (-not $previousVersion -or -not $newVersion) { throw "Installer names must be TaskSmack-<version>-win64.exe or -windows-arm64.exe: $PreviousInstaller, $Installer" }
    if ([version]($previousVersion -replace '-.*$') -ge [version]($newVersion -replace '-.*$')) { throw "$previousVersion isn't older than $newVersion, so this wouldn't be an upgrade." }
    $winget = Get-WingetEntry (Get-Content -LiteralPath $WingetManifest -Raw)
    foreach ($key in 'DisplayName', 'Publisher', 'ProductCode') { if (-not $winget.ContainsKey($key)) { throw "$WingetManifest's AppsAndFeaturesEntries has no $key." } }
    $newTree = Get-FileTree $PackageRoot
    if (-not $newTree.ContainsKey('TaskSmack.exe')) { throw "$PackageRoot has no TaskSmack.exe." }
    $newTree['Uninstall.exe'] = (Get-FileHash -LiteralPath $ExpectedUninstaller -Algorithm SHA256).Hash

    # Nothing of TaskSmack's may be there before: a leftover would pass the checks below for the wrong reason.
    $leftovers = @()
    if (Test-Path -LiteralPath $installDir) { $leftovers += "$installDir exists" }
    if (Test-Path -LiteralPath $configDir) { $leftovers += "$configDir exists" }
    if (@(Get-UninstallEntries).Count) { $leftovers += "a TaskSmack Uninstall key exists: $((Get-UninstallEntries | ForEach-Object Name) -join ', ')" }
    if ($null -ne (Get-RunValue)) { $leftovers += 'the HKCU Run value TaskSmack exists' }
    if ($null -ne (Get-UninstallMark)) { $leftovers += 'the uninstall mark HKCU\Software\TaskSmack Uninstalled exists' }
    Add-Result 'Clean runner' $leftovers 'no TaskSmack install, config, Uninstall key, Run value or uninstall mark before the install'
    $startMenuBefore = @(Get-StartMenuEntries)

    # 1. The previous release.
    Invoke-Silently $PreviousInstaller "The $previousVersion installer"
    $problems = @()
    $entries = @(Get-UninstallEntries)
    if ($entries.Count -ne 1) { $problems += "expected one Programs and Features entry, found $($entries.Count): $(($entries | ForEach-Object Name) -join ', ')" }
    elseif ("$($entries[0].GetValue('DisplayVersion'))" -ne $previousVersion) { $problems += "its DisplayVersion is $($entries[0].GetValue('DisplayVersion')), not $previousVersion" }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { $problems += "no TaskSmack.exe in $installDir" }
    Add-Result "Install $previousVersion" $problems "one entry, $previousVersion, in $installDir"
    $previousTree = Get-FileTree $installDir
    $entry = $entries[0]
    $previousEntry = [ordered]@{ Key = $entry.Name }
    foreach ($name in $entry.GetValueNames()) { $previousEntry[$name] = $entry.GetValue($name) }
    $previousAppPath = Get-AppPath
    Write-Host "Recorded: $($previousTree.Count) files in $installDir; Uninstall key $($entry.Name):"
    $previousEntry.GetEnumerator() | ForEach-Object { Write-Host "  $($_.Key) = $($_.Value)" }
    Write-Host "App Paths: $(if ($previousAppPath) { "$($previousAppPath.Key) = $($previousAppPath.Default)" } else { 'none' })"
    Write-Host "TaskSmack.exe ProductVersion: $((Get-Item -LiteralPath $exe).VersionInfo.ProductVersion)"

    # 2. The user's config, saved by the previous version, and Start at sign-in turned on for that copy.
    New-Item -ItemType Directory -Path $configDir | Out-Null
    $configFile = Join-Path $configDir 'config.toml'
    Copy-Item -LiteralPath $Fixture -Destination $configFile
    $problems = Invoke-HeadlessLaunch -Exe $exe -Label "previous-$previousVersion" -LogDir $logDir
    if (-not $problems.Count) { $problems = Test-FixtureConfig @(Get-Content -LiteralPath $configFile) $FixtureSettings }
    Add-Result "Run $previousVersion once from v0.8.0's config" $problems "it quit by itself and saved $configFile"
    $configTree = Get-FileTree $configDir
    $runCommand = "`"$exe`" --minimized"
    $run = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($RunKeyPath)
    try { $run.SetValue('TaskSmack', $runCommand, [Microsoft.Win32.RegistryValueKind]::String) } finally { $run.Close() }
    New-ItemProperty -LiteralPath $entry.PSPath -Name $SentinelName -Value 1 -PropertyType DWord | Out-Null
    Write-Host "Seeded: $($configTree.Count) file(s) in $configDir; HKCU Run TaskSmack = $runCommand; $SentinelName in $($entry.Name)."

    # 3. This run's installer over it.
    Invoke-Silently $Installer "The $newVersion installer, over $previousVersion,"

    # 4. The upgrade.
    $problems = @()
    $entries = @(Get-UninstallEntries)
    if ($entries.Count -ne 1) { $problems += "expected one Programs and Features entry, found $($entries.Count): $(($entries | ForEach-Object Name) -join ', ')" }
    else {
        $entry = $entries[0]
        if ("$($entry.GetValue('DisplayVersion'))" -ne $newVersion) { $problems += "its DisplayVersion is $($entry.GetValue('DisplayVersion')), not $newVersion" }
        if ($null -ne $entry.GetValue($SentinelName)) { $problems += "its key still holds $SentinelName, so the installer didn't run the old uninstaller" }
        if ($entry.PSChildName -cne $winget.ProductCode) { $problems += "its key is $($entry.PSChildName), not the winget manifest's ProductCode $($winget.ProductCode)" }
        if ("$($entry.GetValue('DisplayName'))" -cne $winget.DisplayName) { $problems += "its DisplayName is $($entry.GetValue('DisplayName')), not the winget manifest's $($winget.DisplayName)" }
        if ("$($entry.GetValue('Publisher'))" -cne $winget.Publisher) { $problems += "its Publisher is $($entry.GetValue('Publisher')), not the winget manifest's $($winget.Publisher)" }
        $uninstallString = "$($entry.GetValue('UninstallString'))".Trim('"')
        if ($uninstallString -ne (Join-Path $installDir 'Uninstall.exe')) { $problems += "its UninstallString is $($entry.GetValue('UninstallString')), not $installDir\Uninstall.exe" }
    }
    Add-Result 'Programs and Features after the upgrade' $problems "one entry, $newVersion, matching the winget manifest's AppsAndFeaturesEntries"

    $installedTree = Get-FileTree $installDir
    $problems = Compare-InstalledTree $newTree $installedTree $previousTree
    $replaced = @($previousTree.Keys | Where-Object { $installedTree.ContainsKey($_) -and $installedTree[$_] -ne $previousTree[$_] }).Count
    $removed = @($previousTree.Keys | Where-Object { -not $newTree.ContainsKey($_) }).Count
    $productVersion = if (Test-Path -LiteralPath $exe -PathType Leaf) { (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion } else { $null }
    if (-not $problems.Count -and $productVersion -ne $newVersion) { $problems += "TaskSmack.exe's ProductVersion is $productVersion, not $newVersion" }
    Add-Result 'Installed files after the upgrade' $problems "exactly the new .zip's $($newTree.Count - 1) files and Uninstall.exe; $replaced of $previousVersion's $($previousTree.Count) replaced, $removed it no longer ships removed; TaskSmack.exe is $productVersion"

    $problems = @()
    $configAfter = Get-FileTree $configDir
    foreach ($name in $configTree.Keys) {
        if (-not $configAfter.ContainsKey($name)) { $problems += "$configDir\$name is gone" }
        elseif ($configAfter[$name] -ne $configTree[$name]) { $problems += "$configDir\$name changed" }
    }
    Add-Result 'User config after the upgrade' $problems "$configDir unchanged"

    $problems = @()
    $runValue = Get-RunValue
    if ($runValue -cne $runCommand) { $problems += "the HKCU Run value TaskSmack is [$runValue], not [$runCommand]" }
    elseif ($installedTree['TaskSmack.exe'] -ne $newTree['TaskSmack.exe']) { $problems += "the exe it names isn't the new one" }
    if ($null -ne (Get-UninstallMark)) { $problems += 'the upgrade left the uninstall mark HKCU\Software\TaskSmack Uninstalled' }
    $shortcuts = @(Get-StartMenuEntries | Where-Object { $startMenuBefore -notcontains $_ -and (Split-Path -Leaf $_) -eq 'TaskSmack.lnk' })
    if ($shortcuts.Count -ne 1) { $problems += "expected one TaskSmack.lnk in the Start menu, found $($shortcuts.Count)" }
    elseif ((Get-ShortcutTarget $shortcuts[0]) -ne $exe) { $problems += "the Start menu's TaskSmack.lnk targets $(Get-ShortcutTarget $shortcuts[0]), not $exe" }
    $appPath = Get-AppPath
    if ($previousAppPath -and (-not $appPath -or "$($appPath.Default)".Trim('"') -ne $exe)) { $problems += "App Paths\TaskSmack.exe is $(if ($appPath) { $appPath.Default } else { 'gone' }), not $exe" }
    Add-Result 'Start at sign-in and shortcuts after the upgrade' $problems "the Run value still starts $exe, now $newVersion; the Start menu shortcut targets it; no uninstall mark"

    # 5. The upgraded TaskSmack, from the user's config.
    $problems = Invoke-HeadlessLaunch -Exe $exe -Label "upgraded-$newVersion" -LogDir $logDir -Strict
    if (-not $problems.Count) { $problems = Test-FixtureConfig @(Get-Content -LiteralPath $configFile) $FixtureSettings }
    Add-Result "Start the upgraded $newVersion" $problems 'it started headless from the user config, quit by itself with exit code 0 and kept v0.8.0''s settings'

    # 6. The uninstall. NSIS's uninstaller copies itself to %TEMP% and runs from there, so the end state is polled for.
    Invoke-Silently (Join-Path $installDir 'Uninstall.exe') 'Uninstall.exe' 120
    $deadline = (Get-Date).AddSeconds(180)
    do {
        $left = @()
        if (Test-Path -LiteralPath $installDir) { $left += "$installDir still holds: $((Get-ChildItem -LiteralPath $installDir -Recurse -Force | ForEach-Object FullName) -join ', ')" }
        $entries = @(Get-UninstallEntries)
        if ($entries.Count) { $left += "the Programs and Features entry is still there: $(($entries | ForEach-Object Name) -join ', ')" }
        $AppKeys | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object { $left += "$_ is still there" }
        $appPath = Get-AppPath
        if ($appPath) { $left += "$($appPath.Key) is still there" }
        $startMenu = @(Get-StartMenuEntries | Where-Object { $startMenuBefore -notcontains $_ })
        if ($startMenu.Count) { $left += "Start menu entries are still there: $($startMenu -join ', ')" }
        $runValue = Get-RunValue
        if ($null -ne $runValue) { $left += "the HKCU Run value TaskSmack is still there: $runValue" }
        if ((Get-UninstallMark) -ne 1) { $left += 'the uninstall mark HKCU\Software\TaskSmack Uninstalled = 1 isn''t there' }
        if (-not $left.Count) { break }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    Add-Result 'Uninstall' $left "$installDir, the Uninstall key, the app's registry key, the Start menu entries and the Run value are gone; the uninstall mark is written; $configDir is the user's and stays"
} catch {
    if (-not $failure) {
        $failure = $_.Exception.Message
        Write-Host "::error::$failure"
        $summary.Add("- **FAILED**: $failure")
    }
} finally {
    $result = if ($failure) { 'FAILED' } else { 'passed' }
    $text = @("### $Title", '', "- Result: $result ($previousVersion, $(Split-Path -Leaf $PreviousInstaller), then $newVersion, $(Split-Path -Leaf $Installer))") + $summary
    if ($env:GITHUB_STEP_SUMMARY) { $text -join "`n" | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append }
    else { $text | ForEach-Object { Write-Host $_ } }
}
if ($failure) { exit 1 }
Write-Host "Upgraded $previousVersion to $newVersion in place, started it, and uninstalled it cleanly."
