<#
.SYNOPSIS
    Regenerate the documentation screenshots (#2827): docs/images/screenshots/windows/<view>-<theme>.webp
.DESCRIPTION
    One TaskSmack launch per shot, sequentially and without injected input: each run gets its own
    TASKSMACK_CONFIG_DIR with a generated config.toml (theme, medium font, 1 s sampling, window size,
    the Command column off: command lines name the machine's users and paths),
    the startup hooks that pick the view (TASKSMACK_WINDOW, _TAB, _MACHINE_TAB, _SELECT_NAME,
    _DETAILS_TAB, _OPEN, and TASKSMACK_DEMO for the shots the startup hooks alone don't reach) and
    TASKSMACK_SCREENSHOT, which writes the frame in-app and quits (CONTRIBUTING.md, "Visual validation
    without input"). Same matrix and names as screenshots.sh.

    Matrix: Overview in every theme; Processes and System in the first dark and the first light theme;
    the machine tab's CPU Cores, GPU and Network and I/O sub-tabs (cores, gpu, network; #2869), Process
    Details (overview, gpu, network, and the Overview with its Modules, Open files and Connections
    sections expanded) for one process, Services, Startup, About and Help in the first theme; and three
    frame sequences written as animated WebPs: overview-live (8 s of the Overview),
    walk-live (10 s: the Processes table, the process selected, its Process Details' Network tab) and
    themes-live (9 s: the Overview through every theme, 1.5 s each), all at 12 fps. The overview-*
    shots and themes-live get a 60 s history window and are captured after 60 s, so their charts are
    full.

    The shots show the machine they run on (its processes, services, startup items, user and host
    name), so run them in a Windows Sandbox or on a clean account, and review every image for names
    before committing it. -WebP needs cwebp and img2webp from libwebp (winget install Google.Libwebp).
.PARAMETER Exe
    TaskSmack binary (default: build\win-release\bin\TaskSmack.exe)
.PARAMETER Out
    Output directory (default: docs\images\screenshots\windows)
.PARAMETER After
    Seconds after startup before each capture (default: 8; the history charts need 5 samples at 1 s).
    overview-* shots and themes-live ignore it: 60 s with a 60 s history window, so their charts are full
.PARAMETER Window
    Window size as WxH (default: 1440x900)
.PARAMETER Themes
    Theme ids; the first is the dark theme, the first containing "light" the light one
.PARAMETER Process
    Process for the Process Details and Processes shots (default: explorer.exe)
.PARAMETER WebP
    Convert each PNG to WebP (cwebp -q 80) and each sequence to an animated WebP
.PARAMETER NoLive
    Skip the frame sequences (overview-live, walk-live, themes-live)
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

$TimeoutAfterCaptureMs = 60000
# -Full shots: a 60 s history window, captured at the hook's maximum delay so the charts are full.
$OverviewHistorySeconds = 60
$OverviewAfter = 60
$LiveFps = 12
$LiveSeconds = 8
$WalkSeconds = 10
# themes-live: each theme for this many tenths of a second, the config's theme first.
$ThemeStepTenths = 15
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
# (";frames=N;fps=F" for a live sequence), then wait for TaskSmack to quit by itself. -Full gives
# the shot the 60 s history window and the 60 s wait.
function Invoke-Shot {
    param([string]$Name, [string]$Theme, [hashtable]$Hooks, [string]$Sequence = "", [switch]$Full)
    $cfg = Join-Path $Work $Name
    $null = New-Item -ItemType Directory -Force -Path $cfg
    $after = if ($Full) { $OverviewAfter } else { $After }
    $history = if ($Full) { "history_max_seconds = $OverviewHistorySeconds`n" } else { "" }
    $toml = "[theme]`nid = '$Theme'`n[font]`nsize = 'medium'`n[sampling]`ninterval_ms = 1000`n$history" +
            "[window]`nwidth = $Width`nheight = $Height`n[process_columns]`ncommand = false`n"
    Set-Content -LiteralPath (Join-Path $cfg "config.toml") -Value $toml -Encoding utf8 -NoNewline
    $shot = Join-Path $PngDir "$Name.png"
    $vars = @{ TASKSMACK_CONFIG_DIR = $cfg; TASKSMACK_WINDOW = $Window; TASKSMACK_SCREENSHOT = "$shot;after=$after$Sequence" } + $Hooks
    foreach ($k in $vars.Keys) { Set-Item -Path "Env:$k" -Value $vars[$k] }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $rc = 'none'
    try {
        $p = Start-Process -FilePath $Exe -WorkingDirectory $cfg -PassThru
        $null = $p.Handle   # cache the handle, or ExitCode is empty after the process has gone
        if ($p.WaitForExit($after * 1000 + $TimeoutAfterCaptureMs)) { $rc = $p.ExitCode } else { $p.Kill(); $rc = 'timeout' }
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

# A frame sequence of $Seconds at $LiveFps, with TASKSMACK_DEMO playing $Demo (its times count from
# the settle point, as the capture's `after` does), joined into <Name>.webp when -WebP and img2webp
# are there; otherwise the frames stay in the work dir for img2webp to join later.
function Invoke-Live {
    param([string]$Name, [string]$Theme, [hashtable]$Hooks, [int]$Seconds, [string]$Demo = "", [switch]$Full)
    if ($Demo) { $Hooks = $Hooks + @{ TASKSMACK_DEMO = $Demo } }
    Invoke-Shot $Name $Theme $Hooks -Sequence ";frames=$($Seconds * $LiveFps);fps=$LiveFps" -Full:$Full
    $frames = @(Get-ChildItem -LiteralPath $PngDir -Filter "$Name-*.png" | Sort-Object Name | ForEach-Object { $_.FullName })
    $img2webp = Get-Command img2webp -ErrorAction SilentlyContinue
    if ($WebP -and $img2webp -and $frames.Count -gt 0) {
        $anim = Join-Path $Out "$Name.webp"
        & $img2webp.Source -loop 0 -mixed -d ([int](1000 / $LiveFps)) -q $WebPQuality @frames -o $anim | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Error "img2webp failed with exit code $LASTEXITCODE" }
        Write-Host "$Name.webp: $($frames.Count) frames, $((Get-Item -LiteralPath $anim).Length) bytes"
    } else {
        Write-Host "$Name frames left in $PngDir (img2webp -loop 0 -d $([int](1000 / $LiveFps)) ... builds the animation)"
    }
}

foreach ($t in $Themes) {
    Invoke-Shot "overview-$t" $t @{ TASKSMACK_TAB = 'overview' } -Full
}
foreach ($t in @($Dark) + $Light) {
    Invoke-Shot "processes-$t" $t @{ TASKSMACK_TAB = 'Processes'; TASKSMACK_SELECT_NAME = $Process }
    Invoke-Shot "system-$t" $t @{ TASKSMACK_TAB = 'System' }
}
# The machine tab's sub-tabs (#2869), after the usual wait: the GPU and network charts need a few samples.
foreach ($sub in 'cores', 'gpu', 'network') {
    Invoke-Shot "$sub-$Dark" $Dark @{ TASKSMACK_TAB = 'overview'; TASKSMACK_MACHINE_TAB = $sub }
}
foreach ($tab in 'overview', 'gpu', 'network') {
    Invoke-Shot "details-$tab-$Dark" $Dark @{ TASKSMACK_SELECT_NAME = $Process; TASKSMACK_DETAILS_TAB = $tab }
}
# The Overview's collapsed sections, each drawn open by a one-step demo script a second after the
# startup selection has settled.
foreach ($section in 'modules', 'openfiles', 'connections') {
    Invoke-Shot "details-$section-$Dark" $Dark @{ TASKSMACK_SELECT_NAME = $Process; TASKSMACK_DETAILS_TAB = 'overview'; TASKSMACK_DEMO = "1:section=$section" }
}
Invoke-Shot "services-$Dark" $Dark @{ TASKSMACK_TAB = 'Services' }
Invoke-Shot "startup-$Dark" $Dark @{ TASKSMACK_TAB = 'Startup' }
Invoke-Shot "about-$Dark" $Dark @{ TASKSMACK_TAB = 'overview'; TASKSMACK_OPEN = 'about' }
Invoke-Shot "help-$Dark" $Dark @{ TASKSMACK_TAB = 'overview'; TASKSMACK_OPEN = 'help' }

if (-not $NoLive) {
    Invoke-Live 'overview-live' $Dark @{ TASKSMACK_TAB = 'overview' } -Seconds $LiveSeconds -Full
    # From the Overview: the Processes table 1 s into the capture, the process selected 2 s later,
    # its Process Details' Network tab 3 s after that.
    $walk = "$($After + 1):tab=processes,$($After + 3):select=name:$Process,$($After + 6):details=network"
    Invoke-Live 'walk-live' $Dark @{ TASKSMACK_TAB = 'overview' } -Seconds $WalkSeconds -Demo $walk
    # The Overview in the first theme, then each of the others for $ThemeStepTenths tenths of a second.
    $steps = @(); $tenths = $OverviewAfter * 10
    foreach ($t in @($Themes | Select-Object -Skip 1)) {
        $tenths += $ThemeStepTenths
        $steps += "$([math]::Floor($tenths / 10)).$($tenths % 10):theme=$t"
    }
    $themesSeconds = [math]::Ceiling($Themes.Count * $ThemeStepTenths / 10)
    Invoke-Live 'themes-live' $Dark @{ TASKSMACK_TAB = 'overview' } -Seconds $themesSeconds -Demo ($steps -join ',') -Full
}

Write-Host ""
$count = @(Get-ChildItem -LiteralPath $Out -File).Count
Write-Host "Wrote $count files to $Out ($script:Failed failed); work files in $Work"
exit $(if ($script:Failed -eq 0) { 0 } else { 1 })
