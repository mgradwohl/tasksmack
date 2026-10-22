<#
.SYNOPSIS
    Regenerate the documentation screenshots (#2827): docs/images/screenshots/windows/<view>-<theme>.webp
.DESCRIPTION
    One TaskSmack launch per shot, sequentially and without injected input: each run gets its own
    TASKSMACK_CONFIG_DIR with a generated config.toml (theme, medium font, 1 s sampling, window size,
    the Command column off: command lines name the machine's users and paths),
    the startup hooks that pick the view (TASKSMACK_WINDOW, _TAB, _SELECT_NAME, _DETAILS_TAB, _OPEN) and
    TASKSMACK_SCREENSHOT, which writes the frame in-app and quits (CONTRIBUTING.md, "Visual validation
    without input"). Same matrix and names as screenshots.sh.

    Matrix: Overview in every theme; Processes and System in the first dark and the first light theme;
    Process Details (overview, gpu, network) for one process, Services, Startup, About and Help in the
    first theme; and overview-live, an 8 s / 12 fps frame sequence written as an animated WebP.

    The shots show the machine they run on (its processes, services, startup items, user and host
    name), so run them in a Windows Sandbox or on a clean account, and review every image for names
    before committing it. -WebP needs cwebp and img2webp from libwebp (winget install Google.Libwebp).
.PARAMETER Exe
    TaskSmack binary (default: build\win-release\bin\TaskSmack.exe)
.PARAMETER Out
    Output directory (default: docs\images\screenshots\windows)
.PARAMETER After
    Seconds after startup before each capture (default: 8; the history charts need 5 samples at 1 s)
.PARAMETER Window
    Window size as WxH (default: 1440x900)
.PARAMETER Themes
    Theme ids; the first is the dark theme, the first containing "light" the light one
.PARAMETER Process
    Process for the Process Details and Processes shots (default: explorer.exe)
.PARAMETER WebP
    Convert each PNG to WebP (cwebp -q 80) and the sequence to an animated WebP
.PARAMETER NoLive
    Skip the overview-live frame sequence
.EXAMPLE
    .\tools\screenshots.ps1 -WebP
.EXAMPLE
    .\tools\screenshots.ps1 -Exe C:\shots\TaskSmack.exe -Out C:\shots\out -Themes windows-dark,nord-light
#>
[CmdletBinding()]
param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\build\win-release\bin\TaskSmack.exe"),
    [string]$Out = (Join-Path $PSScriptRoot "..\docs\images\screenshots\windows"),
    [int]$After = 8,
    [string]$Window = "1440x900",
    [string[]]$Themes = @("windows-dark", "ubuntu-light", "dracula", "nord-light", "gruvbox", "monochrome-light"),
    [string]$Process = "explorer.exe",
    [switch]$WebP,
    [switch]$NoLive
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$TimeoutMs = 60000
$LiveSeconds = 8
$LiveFps = 12
$WebPQuality = 80

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Error "TaskSmack binary not found at $Exe (build the win-release preset or pass -Exe)"
    exit 1
}
$Exe = (Resolve-Path -LiteralPath $Exe).Path
if ($WebP -and -not (Get-Command cwebp -ErrorAction SilentlyContinue)) {
    Write-Error "-WebP needs cwebp and img2webp from libwebp: winget install Google.Libwebp"
    exit 1
}
$Width, $Height = $Window -split 'x', 2
$Work = Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-shots-$PID"
$PngDir = Join-Path $Work "png"
$null = New-Item -ItemType Directory -Force -Path $Out, $PngDir
$Out = (Resolve-Path -LiteralPath $Out).Path
Write-Host "TaskSmack: $Exe"
Write-Host "Output:    $Out  (work: $Work; host: $env:COMPUTERNAME; user: $env:USERNAME)"

$Dark = $Themes[0]
$Light = @($Themes | Where-Object { $_ -like '*light*' -or $_ -eq 'latte' } | Select-Object -First 1)
$script:Failed = 0
Write-Host ("{0,-30} {1,5} {2,8} {3,10}" -f 'name', 'exit', 'seconds', 'bytes')

# One shot: its own config dir, the hooks in $Hooks, TASKSMACK_SCREENSHOT with $Sequence appended
# (";frames=N;fps=F" for the live sequence), then wait for TaskSmack to quit by itself.
function Invoke-Shot {
    param([string]$Name, [string]$Theme, [hashtable]$Hooks, [string]$Sequence = "")
    $cfg = Join-Path $Work $Name
    $null = New-Item -ItemType Directory -Force -Path $cfg
    $toml = "[theme]`nid = '$Theme'`n[font]`nsize = 'medium'`n[sampling]`ninterval_ms = 1000`n" +
            "[window]`nwidth = $Width`nheight = $Height`n[process_columns]`ncommand = false`n"
    Set-Content -LiteralPath (Join-Path $cfg "config.toml") -Value $toml -Encoding utf8 -NoNewline
    $shot = Join-Path $PngDir "$Name.png"
    $vars = @{ TASKSMACK_CONFIG_DIR = $cfg; TASKSMACK_WINDOW = $Window; TASKSMACK_SCREENSHOT = "$shot;after=$After$Sequence" } + $Hooks
    foreach ($k in $vars.Keys) { Set-Item -Path "Env:$k" -Value $vars[$k] }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $rc = 'none'
    try {
        $p = Start-Process -FilePath $Exe -WorkingDirectory $cfg -PassThru
        $null = $p.Handle   # cache the handle, or ExitCode is empty after the process has gone
        if ($p.WaitForExit($TimeoutMs)) { $rc = $p.ExitCode } else { $p.Kill(); $rc = 'timeout' }
    } finally {
        $sw.Stop()
        foreach ($k in $vars.Keys) { Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue }
    }
    $first = if ($Sequence) { Join-Path $PngDir "$Name-0001.png" } else { $shot }
    $bytes = if (Test-Path -LiteralPath $first) { (Get-Item -LiteralPath $first).Length } else { 0 }
    Write-Host ("{0,-30} {1,5} {2,8:N1} {3,10}" -f $Name, $rc, $sw.Elapsed.TotalSeconds, $bytes)
    if ($rc -ne 0 -or $bytes -eq 0) {
        $script:Failed++
        Get-Content -LiteralPath (Join-Path ([System.IO.Path]::GetTempPath()) "tasksmack-debug.log") -Tail 3 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host "    $_" }
        return
    }
    if ($Sequence) { return }
    if ($WebP) {
        & cwebp -quiet -q $WebPQuality $shot -o (Join-Path $Out "$Name.webp")
        if ($LASTEXITCODE -ne 0) { Write-Error "cwebp failed with exit code $LASTEXITCODE on $shot" }
    } else {
        Copy-Item -LiteralPath $shot -Destination (Join-Path $Out "$Name.png")
    }
}

foreach ($t in $Themes) {
    Invoke-Shot "overview-$t" $t @{ TASKSMACK_TAB = 'overview' }
}
foreach ($t in @($Dark) + $Light) {
    Invoke-Shot "processes-$t" $t @{ TASKSMACK_TAB = 'Processes'; TASKSMACK_SELECT_NAME = $Process }
    Invoke-Shot "system-$t" $t @{ TASKSMACK_TAB = 'System' }
}
foreach ($tab in 'overview', 'gpu', 'network') {
    Invoke-Shot "details-$tab-$Dark" $Dark @{ TASKSMACK_SELECT_NAME = $Process; TASKSMACK_DETAILS_TAB = $tab }
}
Invoke-Shot "services-$Dark" $Dark @{ TASKSMACK_TAB = 'Services' }
Invoke-Shot "startup-$Dark" $Dark @{ TASKSMACK_TAB = 'Startup' }
Invoke-Shot "about-$Dark" $Dark @{ TASKSMACK_TAB = 'overview'; TASKSMACK_OPEN = 'about' }
Invoke-Shot "help-$Dark" $Dark @{ TASKSMACK_TAB = 'overview'; TASKSMACK_OPEN = 'help' }

if (-not $NoLive) {
    Invoke-Shot 'overview-live' $Dark @{ TASKSMACK_TAB = 'overview' } -Sequence ";frames=$($LiveSeconds * $LiveFps);fps=$LiveFps"
    $frames = @(Get-ChildItem -LiteralPath $PngDir -Filter 'overview-live-*.png' | Sort-Object Name | ForEach-Object { $_.FullName })
    $img2webp = Get-Command img2webp -ErrorAction SilentlyContinue
    if ($WebP -and $img2webp -and $frames.Count -gt 0) {
        $anim = Join-Path $Out "overview-live.webp"
        & $img2webp.Source -loop 0 -mixed -d ([int](1000 / $LiveFps)) -q $WebPQuality @frames -o $anim | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Error "img2webp failed with exit code $LASTEXITCODE" }
        Write-Host "overview-live.webp: $($frames.Count) frames, $((Get-Item -LiteralPath $anim).Length) bytes"
    } else {
        Write-Host "overview-live frames left in $PngDir (img2webp -loop 0 -d $([int](1000 / $LiveFps)) ... builds the animation)"
    }
}

Write-Host ""
$count = @(Get-ChildItem -LiteralPath $Out -File).Count
Write-Host "Wrote $count files to $Out ($script:Failed failed); work files in $Work"
exit $(if ($script:Failed -eq 0) { 0 } else { 1 })
