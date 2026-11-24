#Requires -Version 7
#!/usr/bin/env pwsh
# The blank line below keeps this comment out of the help block, so Get-Help reads it (#2254).

<#
.SYNOPSIS
    Install, run and uninstall a signed TaskSmack installer in Windows Sandbox with no typing (#4854).
.DESCRIPTION
    The Sandbox install check of docs/dev/release-process.md, stage 6. It writes a .wsb with a read-write
    mapped folder and a LogonCommand that runs the check script inside the Sandbox, starts the Sandbox, waits
    for the result in the mapped folder, closes that Sandbox and prints results.md. The check script:
      - installs the x64 installer silently (/S) and checks the exit code (0 passes; 87 is the unsafe-folder
        refusal of #4669/#4850; 32 means TaskSmack was running);
      - checks C:\Program Files\TaskSmack\TaskSmack.exe and the Uninstall entry (DisplayName TaskSmack,
        DisplayVersion X.Y.Z, Publisher TaskSmack Contributors);
      - starts TaskSmack for 15 s (an OpenGL failure, the Sandbox having no vGPU, is a WARN, not a FAIL);
      - runs Uninstall.exe /S, waits up to 60 s for the folder to go, and checks the entry is gone.
    results.md ends with a RESULT: PASS or RESULT: FAIL line, and this script exits 0 or 1 to match
    (2 when no result arrived in time).

    -WinGet runs the check through SandboxTest.ps1 (pinned to the winget-pkgs commit docs/dev/release-process.md
    pins) with the manifests tools/winget-manifest.ps1 writes for the release, so winget does the install and
    the uninstall. It needs network in the Sandbox. SandboxTest.ps1 asks to confirm a mapped folder outside a
    winget-pkgs clone unless given -WarningAction, so this script passes -WarningAction Continue and is
    non-interactive too; it opens its own Sandbox window with a console, and this script waits for the result
    the same way and closes the Sandbox.

    Without -WinGet the Sandbox has no network: the installer is local.

    DO NOT RUN IT IN CI, and agents must not run it in the daytime without the maintainer's OK: it opens a
    Sandbox window on the desktop. Needs Windows Pro, Enterprise or Education with the Sandbox feature on, and
    no other Sandbox running (only one can). It closes the Sandbox it started, by stopping the Windows Sandbox
    processes, so any other open Sandbox goes too.
.PARAMETER Version
    A published release, X.Y.Z with no leading v. The x64 installer and SHA256SUMS are downloaded with gh, and
    the installer's SHA-256 is checked against SHA256SUMS before anything is installed.
.PARAMETER Installer
    A local TaskSmack-X.Y.Z-win64.exe instead of -Version (a candidate's build, say). The version to expect
    is read from its name.
.PARAMETER WinGet
    Run the check through SandboxTest.ps1 and winget with the release's generated manifests (needs -Version).
.PARAMETER WorkDir
    The folder to map into the Sandbox. Default: a new folder under TEMP. Its path can't contain ' " < > or &.
.PARAMETER TimeoutMinutes
    How long to wait for the result. Default 15.
.PARAMETER WriteOnly
    Write the folder, the check script and the .wsb, print their paths, and stop. Nothing starts.
.EXAMPLE
    pwsh tools/sandbox-install-check.ps1 -Version 0.11.0
.EXAMPLE
    pwsh tools/sandbox-install-check.ps1 -Installer C:\build\TaskSmack-0.11.1-win64.exe
.EXAMPLE
    pwsh tools/sandbox-install-check.ps1 -Version 0.11.0 -WinGet
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Installer,
    [switch]$WinGet,
    [string]$WorkDir,
    [ValidateRange(1, 120)]
    [int]$TimeoutMinutes = 15,
    [switch]$WriteOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = 'mgradwohl/tasksmack'
# The winget-pkgs commit whose Tools/SandboxTest.ps1 docs/dev/release-process.md pins.
$SandboxTestCommit = '1410e3eea47ed72934a257db21cf7a638a862b94'
$RepoRoot = Split-Path -Parent $PSScriptRoot

# The check that runs inside the Sandbox, as text. Windows PowerShell 5.1 runs it (the Sandbox has no pwsh),
# as the signed-in administrator. __INSTALLER__ (a file name beside the script, or empty for -WinGet),
# __VERSION__ (empty to skip the version check) and __OUTDIR__ (a PowerShell expression for the results
# folder) are filled in by New-CheckScript.
$CheckScriptTemplate = @'
# TaskSmack Sandbox install check (tools/sandbox-install-check.ps1). Runs in Windows PowerShell 5.1.
$ErrorActionPreference = 'Continue'
$outDir = __OUTDIR__
$installerName = '__INSTALLER__'
$expectedVersion = '__VERSION__'
$folder = Join-Path $env:ProgramFiles 'TaskSmack'
$arpKey = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\TaskSmack'
$rows = New-Object System.Collections.ArrayList
$script:failed = $false
function Add-Row($Check, $Status, $Detail) {
    [void]$rows.Add("| $Check | $Status | $Detail |")
    if ($Status -eq 'FAIL') { $script:failed = $true }
}
function Get-Arp { if (Test-Path $arpKey) { Get-ItemProperty $arpKey } else { $null } }
try {
__INSTALL__
    Add-Row 'TaskSmack.exe installed' $(if (Test-Path "$folder\TaskSmack.exe") { 'PASS' } else { 'FAIL' }) "$folder\TaskSmack.exe"
    $arp = Get-Arp
    if ($arp) {
        Add-Row 'Uninstall entry' $(if ($arp.DisplayName -eq 'TaskSmack' -and $arp.Publisher -eq 'TaskSmack Contributors') { 'PASS' } else { 'FAIL' }) "DisplayName '$($arp.DisplayName)', Publisher '$($arp.Publisher)'"
        if ($expectedVersion) {
            Add-Row 'DisplayVersion' $(if ($arp.DisplayVersion -eq $expectedVersion) { 'PASS' } else { 'FAIL' }) "'$($arp.DisplayVersion)', expected '$expectedVersion'"
        }
    }
    else { Add-Row 'Uninstall entry' 'FAIL' "$arpKey is missing" }

    if (Test-Path "$folder\TaskSmack.exe") {
        $app = Start-Process "$folder\TaskSmack.exe" -PassThru
        Start-Sleep -Seconds 15
        if (-not $app.HasExited) { Add-Row 'Runs for 15 s' 'PASS' ''; Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
        else { Add-Row 'Runs for 15 s' 'WARN' "exited with $($app.ExitCode) (the Sandbox has no vGPU, so an OpenGL failure is expected)" }
        Start-Sleep -Seconds 2
    }
    # Nothing to uninstall after a failed install: the rows above already say why.
    if ((Test-Path "$folder\TaskSmack.exe") -or (Get-Arp)) {
__UNINSTALL__
        $deadline = (Get-Date).AddSeconds(60)
        while ((Test-Path $folder) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
        Add-Row 'Folder removed' $(if (-not (Test-Path $folder)) { 'PASS' } else { 'FAIL' }) $folder
        Add-Row 'Uninstall entry removed' $(if (-not (Get-Arp)) { 'PASS' } else { 'FAIL' }) ''
    }
    else { Add-Row 'Uninstall' 'SKIP' 'nothing was installed' }
}
catch {
    Add-Row 'Check script' 'FAIL' ($_.Exception.Message -replace '\|', '/')
}
$result = if ($script:failed) { 'FAIL' } else { 'PASS' }
$text = @('# TaskSmack Sandbox install check', '', "Installer: $installerName  Expected version: $expectedVersion", '',
    '| Check | Result | Detail |', '|---|---|---|') + $rows + @('', "RESULT: $result")
Set-Content -Path (Join-Path $outDir 'results.md') -Value $text -Encoding UTF8
Set-Content -Path (Join-Path $outDir 'DONE') -Value "RESULT: $result" -Encoding ASCII
'@

$InstallDirect = @'
    $installer = Join-Path $outDir $installerName
    $p = Start-Process $installer -ArgumentList '/S' -Wait -PassThru
    $note = switch ($p.ExitCode) { 0 { '' } 87 { ' (the unsafe-folder refusal, #4669/#4850)' } 32 { ' (TaskSmack was running)' } default { '' } }
    Add-Row 'Silent install' $(if ($p.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }) "exit code $($p.ExitCode)$note"
'@

$InstallWinGet = @'
    # SandboxTest.ps1 already ran winget install --manifest before this script.
    Add-Row 'winget install' $(if (Test-Path "$folder\TaskSmack.exe") { 'PASS' } else { 'FAIL' }) 'installed by SandboxTest.ps1'
'@

$UninstallDirect = @'
    $u = Start-Process "$folder\Uninstall.exe" -ArgumentList '/S' -Wait -PassThru
    Add-Row 'Silent uninstall' $(if ($u.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }) "exit code $($u.ExitCode)"
'@

$UninstallWinGet = @'
    $u = Start-Process 'winget' -ArgumentList 'uninstall --name TaskSmack --exact --silent --accept-source-agreements' -Wait -PassThru -NoNewWindow
    Add-Row 'winget uninstall' $(if ($u.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }) "exit code $($u.ExitCode)"
'@

function New-CheckScript {
    # The in-Sandbox script text. -OutDirExpression is PowerShell source for the results folder as the
    # Sandbox sees it ('$here' beside the script, or the mapped desktop folder under -WinGet).
    param([string]$InstallerName, [string]$ExpectedVersion, [switch]$ViaWinGet, [string]$OutDirExpression)
    $install = if ($ViaWinGet) { $InstallWinGet } else { $InstallDirect }
    $uninstall = if ($ViaWinGet) { $UninstallWinGet } else { $UninstallDirect }
    return $CheckScriptTemplate.Replace('__OUTDIR__', $OutDirExpression).Replace('__INSTALLER__', $InstallerName).
        Replace('__VERSION__', $ExpectedVersion).Replace('__INSTALL__', $install).Replace('__UNINSTALL__', $uninstall)
}

function New-SandboxConfig {
    # The .wsb text. The installer is local, so networking is off unless winget needs it.
    param([string]$HostFolder, [string]$ScriptName, [switch]$Network)
    $networking = if ($Network) { 'Enable' } else { 'Disable' }
    return @"
<Configuration>
  <Networking>$networking</Networking>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$HostFolder</HostFolder>
      <SandboxFolder>C:\ws</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ws\$ScriptName</Command>
  </LogonCommand>
</Configuration>
"@
}

function Assert-SandboxArguments {
    param([string]$Version, [string]$Installer, [switch]$WinGet, [string]$WorkDir)
    if ($Version -and $Installer) { throw 'Give -Version or -Installer, not both.' }
    if (-not $Version -and -not $Installer) { throw 'Give -Version X.Y.Z (a published release) or -Installer <path to TaskSmack-X.Y.Z-win64.exe>.' }
    if ($Version -and $Version -notmatch '^\d+\.\d+\.\d+$') { throw "-Version must be X.Y.Z with no leading v, not '$Version'." }
    if ($WinGet -and -not $Version) { throw '-WinGet needs -Version: the manifests are made from the published release.' }
    if ($Installer) {
        if (-not (Test-Path -LiteralPath $Installer -PathType Leaf)) { throw "-Installer '$Installer' does not exist." }
        if ((Split-Path -Leaf $Installer) -notmatch '^TaskSmack-\d+\.\d+\.\d+-win64\.exe$') { throw "-Installer must be named TaskSmack-X.Y.Z-win64.exe (the x64 installer), not '$(Split-Path -Leaf $Installer)'." }
    }
    if ($WorkDir -and $WorkDir -match '[''"<>&]') { throw "-WorkDir can't contain ' `" < > or &: $WorkDir" }
}

function Stop-SandboxProcesses {
    foreach ($name in 'WindowsSandboxRemoteSession', 'WindowsSandbox', 'WindowsSandboxClient') {
        Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    # The session takes a moment to go; wait for it, so the next run doesn't meet a Sandbox still closing.
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Process -Name 'WindowsSandboxRemoteSession', 'WindowsSandbox', 'WindowsSandboxClient' -ErrorAction SilentlyContinue) -and
        (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
}

function Wait-ForResult {
    param([string]$Folder, [int]$Minutes)
    $done = Join-Path $Folder 'DONE'
    $deadline = (Get-Date).AddMinutes($Minutes)
    while (-not (Test-Path -LiteralPath $done) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }
    return (Test-Path -LiteralPath $done)
}

# Dot-sourcing (the tests) loads the functions without running anything.
if ($MyInvocation.InvocationName -eq '.') { return }

Assert-SandboxArguments -Version $Version -Installer $Installer -WinGet:$WinGet -WorkDir $WorkDir
if (-not $WriteOnly) {
    if (-not $IsWindows) { throw 'Windows Sandbox needs Windows.' }
    if (-not (Get-Command WindowsSandbox -ErrorAction SilentlyContinue)) { throw 'Windows Sandbox is not installed (Enable-WindowsOptionalFeature -FeatureName Containers-DisposableClientVM -All -Online, then restart).' }
    if (Get-Process -Name WindowsSandboxClient, WindowsSandboxRemoteSession -ErrorAction SilentlyContinue) { throw 'A Windows Sandbox is already running. Close it first: only one can run.' }
}

if (-not $WorkDir) { $WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-sandbox-check-$([guid]::NewGuid().ToString('N').Substring(0, 8))" }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
if ($WorkDir -match '[''"<>&]') { throw "The work folder can't contain ' `" < > or &: $WorkDir" }
foreach ($stale in 'DONE', 'results.md') { Remove-Item -LiteralPath (Join-Path $WorkDir $stale) -ErrorAction SilentlyContinue }

if ($Version) {
    $installerName = "TaskSmack-$Version-win64.exe"
    if (-not (Test-Path -LiteralPath (Join-Path $WorkDir $installerName)) -and -not $WriteOnly) {
        & gh release download "v$Version" --repo $Repo --pattern $installerName --pattern SHA256SUMS --dir $WorkDir --clobber
        if ($LASTEXITCODE -ne 0) { throw "gh release download v$Version failed." }
    }
    if (-not $WriteOnly) {
        $line = Get-Content -LiteralPath (Join-Path $WorkDir 'SHA256SUMS') | Where-Object { $_ -match "\s\*?$([regex]::Escape($installerName))$" } | Select-Object -First 1
        if (-not $line) { throw "$installerName is not in SHA256SUMS." }
        $expected = ($line -split '\s+')[0].ToLowerInvariant()
        if ((Get-FileHash -LiteralPath (Join-Path $WorkDir $installerName) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw "$installerName does not match SHA256SUMS." }
    }
    $expectedVersion = $Version
}
else {
    $installerName = Split-Path -Leaf $Installer
    Copy-Item -LiteralPath $Installer -Destination (Join-Path $WorkDir $installerName) -Force
    $expectedVersion = ([regex]::Match($installerName, '^TaskSmack-(\d+\.\d+\.\d+)-win64\.exe$')).Groups[1].Value
}

$scriptName = 'sandbox-install-check-inside.ps1'
$folderLeaf = Split-Path -Leaf $WorkDir
if ($WinGet) {
    $outExpr = "(Join-Path `$env:USERPROFILE 'Desktop\$folderLeaf')"
    $checkText = New-CheckScript -InstallerName "winget manifest $expectedVersion" -ExpectedVersion $expectedVersion -ViaWinGet -OutDirExpression $outExpr
}
else {
    $checkText = New-CheckScript -InstallerName $installerName -ExpectedVersion $expectedVersion -OutDirExpression '(Split-Path -Parent $MyInvocation.MyCommand.Path)'
}
Set-Content -LiteralPath (Join-Path $WorkDir $scriptName) -Value $checkText -Encoding utf8
if (-not $WinGet) {
    Set-Content -LiteralPath (Join-Path $WorkDir 'check.wsb') -Value (New-SandboxConfig -HostFolder $WorkDir -ScriptName $scriptName) -Encoding utf8
}
Write-Host "Work folder: $WorkDir"
if ($WriteOnly) { Write-Host 'Wrote the check script and the .wsb; not starting anything (-WriteOnly).'; return }

try {
    if ($WinGet) {
        $sandboxTest = Join-Path $WorkDir 'SandboxTest.ps1'
        Invoke-WebRequest "https://raw.githubusercontent.com/microsoft/winget-pkgs/$SandboxTestCommit/Tools/SandboxTest.ps1" -OutFile $sandboxTest
        $release = Join-Path $WorkDir "v$Version"
        & gh release download "v$Version" --repo $Repo --pattern SHA256SUMS --dir $release --clobber
        if ($LASTEXITCODE -ne 0) { throw "gh release download v$Version failed." }
        $date = (& gh release view "v$Version" --repo $Repo --json publishedAt --jq '.publishedAt[:10]')
        & (Join-Path $PSScriptRoot 'winget-manifest.ps1') -Version $Version -ReleaseDir $release -ReleaseDate $date
        $manifest = Join-Path $release "winget\manifests\t\TaskSmack\TaskSmack\$Version"
        Push-Location $WorkDir
        # -WarningAction Continue: without it, SandboxTest.ps1 asks "Continue with this operation?" about a mapped
        # folder outside a winget-pkgs clone (its OnMappedFolderWarning defaults to Inquire).
        try { & $sandboxTest -Manifest $manifest -MapFolder $WorkDir -Script ([scriptblock]::Create($checkText)) -WarningAction Continue }
        finally { Pop-Location }
    }
    else {
        Start-Process (Join-Path $WorkDir 'check.wsb')
    }
    Write-Host "Waiting up to $TimeoutMinutes min for the result..."
    if (-not (Wait-ForResult -Folder $WorkDir -Minutes $TimeoutMinutes)) {
        Write-Host "No result after $TimeoutMinutes min. Closing the Sandbox; the work folder is kept: $WorkDir"
        exit 2
    }
}
finally {
    Stop-SandboxProcesses
}
Get-Content -LiteralPath (Join-Path $WorkDir 'results.md') | Write-Host
$passed = (Get-Content -LiteralPath (Join-Path $WorkDir 'DONE') -Raw) -match 'RESULT: PASS'
Write-Host "Work folder kept: $WorkDir"
exit $(if ($passed) { 0 } else { 1 })
