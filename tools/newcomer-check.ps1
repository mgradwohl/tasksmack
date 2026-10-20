<#
.SYNOPSIS
    Walk the documented Windows newcomer path on a clean machine and report where it breaks (#2618).
.DESCRIPTION
    Follows README.md, then CONTRIBUTING.md, the way a new contributor on a fresh Windows does:
    read the docs on GitHub, install git, clone, run tools/setup-dev.ps1 and
    tools/check-prereqs.ps1, install the pre-commit hooks, run the win-dev workflow preset (configure,
    build, ctest), launch TaskSmack once with a temporary TASKSMACK_CONFIG_DIR, and run the documented
    benchmark and ETW profiling steps.

    The script only automates what the docs say. Before each step it checks that the docs still give
    the command it is about to run; if they no longer do, the docs and this script disagree and the
    run stops there with a FAIL. A step the docs leave out or get wrong (a tool they never say how to
    install, a command that fails as written) is a FINDING. By default the first FINDING stops the
    run; -ContinueOnFinding records it, applies the most obvious workaround and goes on, so one run
    shows every finding.

    Every step's status (PASS, FINDING, FAIL, SKIP), duration and notes go to results.md and
    results.json in -OutDir, rewritten after each step; each command's output goes to a log file
    there. When the run ends, -OutDir\DONE holds the overall result.

    Runs on Windows PowerShell 5.1, which is all a clean Windows has, as well as PowerShell 7.
    Designed for Windows Sandbox (a .wsb that maps this script in read-only, maps an output folder,
    and runs it as the LogonCommand), but works on any throwaway Windows 10/11 machine. It installs
    software machine-wide, so do not run it on a machine you care about.
.PARAMETER WorkDir
    Where the repository is cloned. Must not already hold a clone.
.PARAMETER OutDir
    Where results, logs and the DONE marker go.
.PARAMETER Ref
    The branch or tag to read the docs from and clone. Defaults to main.
.PARAMETER RepoUrl
    The repository to clone; the docs' own clone URL is checked against it.
.PARAMETER ContinueOnFinding
    Record findings and keep going instead of stopping at the first one.
.PARAMETER WindowsSandbox
    Prepare Windows Sandbox first: install winget (the sandbox has no Microsoft Store, so no winget;
    an ordinary Windows 10/11 has it), and before the launch put CI's pinned Mesa llvmpipe beside
    TaskSmack.exe, since a sandbox without vGPU has only OpenGL 1.1. Not newcomer steps; reported
    as SETUP. Use <VGpu>Disable</VGpu> and MemoryInMB 8192 in the .wsb. Expect setup-dev.ps1
    to take hours there: Windows Installer spends 2 minutes verifying each signed .msi in the
    sandbox, and the Visual Studio Build Tools install runs about 126 of them.
.PARAMETER BuildParallelLevel
    Sets CMAKE_BUILD_PARALLEL_LEVEL for the builds, to keep a sandbox from taking every core of the
    host. 0 (the default) leaves it unset, as a newcomer would.
.PARAMETER SetupTimeoutMinutes
    How long tools/setup-dev.ps1 may run before the check records a FINDING, copies the installers'
    logs to OutDir and stops. Default 45, ample on an ordinary machine. Windows Sandbox needs about
    360 (see -WindowsSandbox).
.PARAMETER SkipPerf
    Skip the benchmark and ETW steps.
.PARAMETER ShutdownWhenDone
    Shut the machine down at the end (closes Windows Sandbox).
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File newcomer-check.ps1 -OutDir C:\out -ContinueOnFinding
.EXAMPLE
    # Windows Sandbox: a .wsb that maps a folder holding only this script read-only to
    # C:\newcomer-in and an empty folder writable to C:\newcomer-out, with Networking enabled,
    # MemoryInMB 8192, VGpu Disable, and this LogonCommand (one line; it shuts the sandbox down at the end):
    cmd.exe /c "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\newcomer-in\newcomer-check.ps1 -WorkDir C:\work -OutDir C:\newcomer-out -ContinueOnFinding -WindowsSandbox -SetupTimeoutMinutes 360 > C:\newcomer-out\console.log 2>&1 & shutdown /s /t 60"
    # Then watch progress.log and wait for DONE in the output folder on the host.
#>
[CmdletBinding()]
param(
    [string]$WorkDir = (Join-Path $env:USERPROFILE 'newcomer'),
    [string]$OutDir = (Join-Path $env:USERPROFILE 'newcomer-results'),
    [string]$Ref = 'main',
    [string]$RepoUrl = 'https://github.com/mgradwohl/tasksmack.git',
    [switch]$ContinueOnFinding,
    [switch]$WindowsSandbox,
    [int]$BuildParallelLevel = 0,
    [int]$SetupTimeoutMinutes = 45,
    [switch]$SkipPerf,
    [switch]$ShutdownWhenDone
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# A -ExecutionPolicy on the command line that started this script is passed to every child
# PowerShell through this variable. Clear it, so the newcomer's commands run under the machine's
# own policy, as they would in a terminal the newcomer opened.
Remove-Item Env:PSExecutionPolicyPreference -ErrorAction SilentlyContinue

$null = New-Item -ItemType Directory -Force -Path $OutDir
$RepoDir = Join-Path $WorkDir 'tasksmack'
$DocsDir = Join-Path $OutDir 'docs'
$script:Results = New-Object System.Collections.ArrayList
$script:StepNumber = 0
$script:Stopped = $false
$script:RunStart = Get-Date
$script:Commit = '(not cloned)'
$RawBase = ($RepoUrl -replace '\.git$', '') -replace '^https://github\.com/', 'https://raw.githubusercontent.com/'

$script:ExtraPath = @()

# Write or append a results file, retrying while something else (a host watching a Windows Sandbox
# mapped folder) has it open. Losing a progress line must never fail a step.
function Write-OutFile {
    param([string]$Name, [string]$Text, [switch]$Append)
    $path = Join-Path $OutDir $Name
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    for ($i = 0; $i -lt 20; $i++) {
        try {
            if ($Append) { [IO.File]::AppendAllText($path, $Text, $utf8) } else { [IO.File]::WriteAllText($path, $Text, $utf8) }
            return
        }
        catch { Start-Sleep -Milliseconds 250 }
    }
    Write-Warning "could not write $path"
}

function Write-CheckLog {
    param([string]$Message)
    $line = '{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $Message
    Write-Host $line
    Write-OutFile -Name 'progress.log' -Text "$line`r`n" -Append
}

function Write-Result {
    $md = New-Object System.Text.StringBuilder
    $null = $md.AppendLine('# Newcomer check (Windows)')
    $null = $md.AppendLine('')
    $null = $md.AppendLine("Ref: $Ref, commit: $script:Commit, started: $($script:RunStart.ToString('u'))")
    $null = $md.AppendLine('')
    $null = $md.AppendLine('| # | Step | Result | Time | Notes |')
    $null = $md.AppendLine('|---|---|---|---|---|')
    foreach ($r in $script:Results) {
        $time = '{0:hh\:mm\:ss}' -f [TimeSpan]::FromSeconds([math]::Round($r.Seconds))
        $notes = ($r.Notes -join ' ') -replace '\|', '\|' -replace "`r?`n", ' '
        $null = $md.AppendLine("| $($r.Number) | $($r.Step) | $($r.Status) | $time | $notes |")
    }
    Write-OutFile -Name 'results.md' -Text $md.ToString()
    $json = [ordered]@{ ref = $Ref; commit = $script:Commit; started = $script:RunStart.ToString('o'); steps = $script:Results }
    Write-OutFile -Name 'results.json' -Text ($json | ConvertTo-Json -Depth 5)
}

# The step being run. Each step adds notes and findings to it; Invoke-Step records the result.
$script:Current = $null

function Add-Note {
    param([string]$Text)
    $null = $script:Current.Notes.Add($Text)
    Write-CheckLog "  note: $Text"
}

# A place the docs or scripts trip a newcomer. Stops the run unless -ContinueOnFinding.
function Add-Finding {
    param([string]$Text)
    $script:Current.Findings++
    $null = $script:Current.Notes.Add("FINDING: $Text")
    Write-CheckLog "  FINDING: $Text"
    if (-not $ContinueOnFinding) {
        throw "STOPRUN: $Text"
    }
}

function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Body,
        [switch]$Setup
    )
    if ($script:Stopped) { return }
    $script:StepNumber++
    $script:Current = [pscustomobject]@{
        Number   = $script:StepNumber
        Step     = $Name
        Status   = 'RUNNING'
        Seconds  = 0
        Notes    = New-Object System.Collections.ArrayList
        Findings = 0
    }
    $null = $script:Results.Add($script:Current)
    Write-CheckLog "STEP $($script:StepNumber): $Name"
    Write-Result
    $start = Get-Date
    try {
        $skipReason = @(& $Body) | Select-Object -Last 1
        if ($skipReason -is [string] -and $skipReason.StartsWith('SKIP:')) {
            $script:Current.Status = 'SKIP'
            $null = $script:Current.Notes.Add($skipReason.Substring(5).Trim())
        }
        elseif ($script:Current.Findings -gt 0) { $script:Current.Status = 'FINDING' }
        elseif ($Setup) { $script:Current.Status = 'SETUP' }
        else { $script:Current.Status = 'PASS' }
    }
    catch {
        $message = $_.Exception.Message
        # NOSTOP: a failure the later steps don't depend on (failing tests in a finished build).
        if ($message.StartsWith('NOSTOP:')) {
            $script:Current.Status = 'FAIL'
            $null = $script:Current.Notes.Add("FAIL: $($message.Substring(7).Trim())")
        }
        elseif ($message.StartsWith('STOPRUN:')) {
            $script:Stopped = $true
            $script:Current.Status = 'FINDING'
            $null = $script:Current.Notes.Add('Stopped at the first finding (rerun with -ContinueOnFinding to see the rest).')
        }
        else {
            $script:Stopped = $true
            $script:Current.Status = 'FAIL'
            $null = $script:Current.Notes.Add("FAIL: $message")
        }
    }
    $script:Current.Seconds = ((Get-Date) - $start).TotalSeconds
    Write-CheckLog "  -> $($script:Current.Status) in $([math]::Round($script:Current.Seconds)) s"
    Write-Result
}

# Re-read Path and the other environment variables from the registry: what opening a new terminal
# does after an installer changed them.
function Update-SessionEnvironment {
    foreach ($scope in 'Machine', 'User') {
        $vars = [Environment]::GetEnvironmentVariables($scope)
        foreach ($name in $vars.Keys) {
            if ($name -in 'Path', 'PSModulePath', 'TEMP', 'TMP', 'USERNAME') { continue }
            Set-Item -Path "Env:$name" -Value $vars[$name]
        }
    }
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = (@($machinePath, $userPath) + $script:ExtraPath | Where-Object { $_ }) -join ';'
}

# Run a command in a new shell, as the newcomer typing it into a terminal: Windows PowerShell
# (powershell.exe, the default on a clean Windows) or PowerShell 7 (pwsh). Output goes to
# <OutDir>\<LogName>.log. Returns the exit code; a timeout kills the process tree and throws.
function Invoke-Shell {
    param(
        [ValidateSet('powershell', 'pwsh')]
        [string]$Shell,
        [string]$Command,
        [string]$LogName,
        [int]$TimeoutMinutes = 30,
        [string]$WorkingDirectory = $RepoDir,
        # Only for machine setup that is not a newcomer step; newcomer commands run under the
        # machine's own execution policy.
        [string]$ExecutionPolicy
    )
    if (-not (Test-Path -LiteralPath $WorkingDirectory)) { $WorkingDirectory = $WorkDir }
    # UTF-8 output, so the log keeps check-prereqs.ps1's marks. The exit code is the last native
    # command's, or 1 when the last command was a PowerShell error (such as a command not found).
    $prefix = "try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new(`$false) } catch { }; `$ErrorActionPreference = 'Continue'; Set-Location -LiteralPath '$WorkingDirectory'"
    $suffix = '$ok = $?; if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE } elseif (-not $ok) { exit 1 } else { exit 0 }'
    $full = "$prefix`n$Command`n$suffix"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($full))
    $log = Join-Path $OutDir "$LogName.log"
    $errLog = Join-Path $OutDir "$LogName.stderr.log"
    $exe = (Get-Command $Shell -ErrorAction Stop).Source
    Write-CheckLog "  [$Shell] $($Command -replace "`r?`n", ' ; ')  (log: $LogName.log)"
    $shellArgs = @('-NoLogo', '-NoProfile', '-NonInteractive', '-OutputFormat', 'Text')
    if ($ExecutionPolicy) { $shellArgs += @('-ExecutionPolicy', $ExecutionPolicy) }
    $shellArgs += @('-EncodedCommand', $encoded)
    $p = Start-Process -FilePath $exe -ArgumentList $shellArgs `
        -WorkingDirectory $WorkingDirectory -RedirectStandardOutput $log -RedirectStandardError $errLog -NoNewWindow -PassThru
    $null = $p.Handle  # makes ExitCode readable after the wait on Windows PowerShell
    # A heartbeat every 5 minutes, naming the busiest processes, so a long or stuck step shows
    # what it is doing.
    $started = Get-Date
    $deadline = $started.AddMinutes($TimeoutMinutes)
    while (-not $p.WaitForExit(300000)) {
        $busy = Get-Process | Where-Object { $_.CPU } | Sort-Object CPU -Descending | Select-Object -First 4 |
            ForEach-Object { '{0} {1:N0}s' -f $_.Name, $_.CPU }
        Write-CheckLog ('  ... {0:N0} min, busiest: {1}' -f ((Get-Date) - $started).TotalMinutes, ($busy -join ', '))
        Save-Diagnostic -Prefix $LogName
        if ((Get-Date) -ge $deadline) {
            & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
            throw "timed out after $TimeoutMinutes min: $Command"
        }
    }
    $p.WaitForExit()
    if ((Test-Path -LiteralPath $errLog) -and (Get-Item -LiteralPath $errLog).Length -gt 0) {
        $stderrText = [IO.File]::ReadAllText($errLog)
        Write-OutFile -Name "$LogName.log" -Text "`r`n--- stderr ---`r`n$stderrText" -Append
    }
    Remove-Item -LiteralPath $errLog -ErrorAction SilentlyContinue
    return $p.ExitCode
}

# Copy the installers' own logs (winget's, and the Visual Studio installer's dd_*.log in TEMP) into
# OutDir, for a step that failed or timed out inside an installer.
function Save-Diagnostic {
    param([string]$Prefix)
    $dest = Join-Path $OutDir "$Prefix-diagnostics"
    $null = New-Item -ItemType Directory -Force -Path $dest
    $sources = @(
        (Join-Path $env:LOCALAPPDATA 'Packages\Microsoft.DesktopAppInstaller_8wekyb3d8bbwe\LocalState\DiagOutputDir\*.log'),
        (Join-Path $env:TEMP 'dd_*.log'),
        (Join-Path $env:TEMP 'WinGet\*.log')
    )
    foreach ($source in $sources) {
        Get-ChildItem -Path $source -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
            Select-Object -First 10 | Copy-Item -Destination $dest -ErrorAction SilentlyContinue
    }
}

function Get-LogTail {
    param([string]$LogName, [int]$Lines = 15)
    $log = Join-Path $OutDir "$LogName.log"
    if (-not (Test-Path -LiteralPath $log)) { return '' }
    return ((Get-Content -LiteralPath $log -Tail $Lines -Encoding utf8) -join "`n")
}

# The text of a doc as the newcomer reads it: from GitHub before the clone, from the clone after.
function Get-DocText {
    param([string]$Name)
    $local = Join-Path $RepoDir $Name
    if (Test-Path -LiteralPath $local) { return Get-Content -LiteralPath $local -Raw -Encoding utf8 }
    return Get-Content -LiteralPath (Join-Path $DocsDir $Name) -Raw -Encoding utf8
}

# Whether a doc contains $Text, ignoring how the doc wraps it: runs of whitespace, line breaks
# included, compare equal to one space. Markdown prose wraps anywhere, so a phrase that reads
# whole on GitHub can sit across two lines in the file.
function Test-DocText {
    param([string]$Doc, [string]$Text)
    return ((Get-DocText $Doc) -replace '\s+', ' ').Contains(($Text -replace '\s+', ' '))
}

# The docs must still give the command this script is about to run; if not, they and this script
# disagree, and the script has to be updated to follow them.
function Assert-DocText {
    param([string]$Doc, [string]$Text)
    if (-not (Test-DocText $Doc $Text)) {
        throw "$Doc no longer says '$Text'. The docs and tools/newcomer-check.ps1 disagree; update the script to follow the docs."
    }
}

function Test-DocMatch {
    param([string]$Doc, [string]$Pattern)
    return [regex]::IsMatch((Get-DocText $Doc), $Pattern)
}

function Install-WithWinGet {
    param([string]$Id, [string]$LogName)
    $code = Invoke-Shell -Shell powershell -LogName $LogName -TimeoutMinutes 30 -WorkingDirectory $WorkDir `
        -Command "winget install --id $Id --exact --source winget --silent --accept-package-agreements --accept-source-agreements"
    if ($code -ne 0) { throw "winget install $Id failed with exit code $code`n$(Get-LogTail $LogName)" }
    Update-SessionEnvironment
}

# PrintWindow capture of a window, as CONTRIBUTING's "Visual validation without input" does it.
function Save-WindowCapture {
    param([IntPtr]$Handle, [string]$Path)
    if (-not ('NewcomerCheck.Capture' -as [type])) {
        Add-Type -AssemblyName System.Drawing
        Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
namespace NewcomerCheck {
    public static class Capture {
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
        public static string Save(IntPtr h, string path) {
            RECT r; if (!GetWindowRect(h, out r)) return "GetWindowRect failed";
            int w = r.Right - r.Left, ht = r.Bottom - r.Top;
            if (w <= 0 || ht <= 0) return "empty window rect";
            using (var bmp = new Bitmap(w, ht)) {
                using (var g = Graphics.FromImage(bmp)) {
                    IntPtr hdc = g.GetHdc();
                    try { PrintWindow(h, hdc, 2); } finally { g.ReleaseHdc(hdc); }
                }
                bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
                // How many distinct colours a coarse grid sees: 1 means a blank capture.
                var seen = new System.Collections.Generic.HashSet<int>();
                for (int y = 0; y < ht; y += Math.Max(1, ht / 40))
                    for (int x = 0; x < w; x += Math.Max(1, w / 40))
                        seen.Add(bmp.GetPixel(x, y).ToArgb());
                return w + "x" + ht + ", " + seen.Count + " distinct colours sampled";
            }
        }
    }
}
'@
    }
    $info = [NewcomerCheck.Capture]::Save($Handle, $Path)
    Add-Note "capture: $info"
}

$null = New-Item -ItemType Directory -Force -Path $WorkDir, $DocsDir
if (Test-Path -LiteralPath $RepoDir) { throw "$RepoDir already exists; give -WorkDir a fresh directory." }
Remove-Item -LiteralPath (Join-Path $OutDir 'DONE') -ErrorAction SilentlyContinue
Write-CheckLog "Newcomer check: ref $Ref, work dir $WorkDir, PowerShell $($PSVersionTable.PSVersion), $([Environment]::OSVersion.VersionString)"
if ($BuildParallelLevel -gt 0) { $env:CMAKE_BUILD_PARALLEL_LEVEL = "$BuildParallelLevel" }

# --- 0. Machine (not a newcomer step) ---------------------------------------------------------
Invoke-Step -Setup 'Machine: clean environment' {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    $cpu = (Get-CimInstance Win32_ComputerSystem)
    Add-Note "$([Environment]::OSVersion.Version), $($cpu.NumberOfLogicalProcessors) logical CPUs, $([math]::Round($cpu.TotalPhysicalMemory / 1GB)) GB, admin=$isAdmin, execution policy (LocalMachine)=$(Get-ExecutionPolicy -Scope LocalMachine)"
    $present = @('git', 'pwsh', 'cmake', 'ninja', 'clang', 'python', 'py', 'winget') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue }
    Add-Note "Already on PATH: $(if ($present) { $present -join ', ' } else { 'none of git, pwsh, cmake, ninja, clang, python, py, winget' })"
    if ($BuildParallelLevel -gt 0) { Add-Note "CMAKE_BUILD_PARALLEL_LEVEL=$BuildParallelLevel (host-load cap, not a newcomer step)" }
    # A freshly started VM can take a moment to get its network.
    for ($i = 1; $i -le 24; $i++) {
        try {
            $null = Invoke-WebRequest -UseBasicParsing -Method Head -Uri 'https://github.com' -TimeoutSec 10
            return
        }
        catch { Start-Sleep -Seconds 5 }
    }
    throw 'no network: https://github.com was unreachable for 2 minutes'
}

if ($WindowsSandbox) {
    Invoke-Step -Setup 'Machine: install winget (Windows Sandbox has none)' {
        if (Get-Command winget -ErrorAction SilentlyContinue) { return 'SKIP: winget is already installed' }
        # Microsoft's documented way to add winget to Windows Sandbox:
        # https://learn.microsoft.com/windows/package-manager/winget/#install-winget-on-windows-sandbox
        $code = Invoke-Shell -Shell powershell -ExecutionPolicy Bypass -LogName '00-winget-bootstrap' -TimeoutMinutes 20 -WorkingDirectory $WorkDir -Command @'
$ProgressPreference = 'SilentlyContinue'
Install-PackageProvider -Name NuGet -Force | Out-Null
Install-Module -Name Microsoft.WinGet.Client -Force -Repository PSGallery | Out-Null
Repair-WinGetPackageManager -AllUsers
'@
        $script:ExtraPath += Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps'
        Update-SessionEnvironment
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
            throw "winget bootstrap failed (exit $code)`n$(Get-LogTail '00-winget-bootstrap')"
        }
        Add-Note "winget $(& winget --version)"
    }
}

# --- 1. Read the docs ------------------------------------------------------------------------
Invoke-Step 'Read README.md and CONTRIBUTING.md on GitHub' {
    foreach ($doc in 'README.md', 'CONTRIBUTING.md') {
        Invoke-WebRequest -UseBasicParsing -Uri "$RawBase/$Ref/$doc" -OutFile (Join-Path $DocsDir $doc)
    }
    # README's route to the contributor docs.
    if (-not (Test-DocMatch 'README.md' '\(CONTRIBUTING\.md\)')) {
        Add-Finding 'README.md does not link CONTRIBUTING.md.'
    }
}

# --- 2. git ----------------------------------------------------------------------------------
Invoke-Step 'Install git' {
    if (Get-Command git -ErrorAction SilentlyContinue) { return 'SKIP: git is already installed' }
    $documented = (Test-DocMatch 'CONTRIBUTING.md' 'winget install[^\r\n`]*Git\.Git') -or (Test-DocMatch 'CONTRIBUTING.md' 'git-scm\.com')
    if (-not $documented) {
        Add-Finding 'CONTRIBUTING.md starts with git clone but never says how to get git on Windows (installed here with winget install Git.Git).'
    }
    Install-WithWinGet -Id 'Git.Git' -LogName '02-install-git'
    Add-Note "$(& git --version)"
}

# --- 3. Clone (Quick Start) ------------------------------------------------------------------
Invoke-Step 'Clone (Quick Start)' {
    Assert-DocText 'CONTRIBUTING.md' "git clone $RepoUrl"
    $code = Invoke-Shell -Shell powershell -LogName '03-clone' -TimeoutMinutes 20 -WorkingDirectory $WorkDir `
        -Command "git clone --branch $Ref $RepoUrl"
    if ($code -ne 0) { throw "git clone failed ($code)`n$(Get-LogTail '03-clone')" }
    $script:Commit = (& git -C $RepoDir rev-parse HEAD).Trim()
    Add-Note "commit $script:Commit"
}

# --- 4. Quick Start's Python and pre-commit lines, in a Windows terminal ---------------------
Invoke-Step 'Quick Start on Windows' {
    $quickStart = [regex]::Match((Get-DocText 'CONTRIBUTING.md'), '(?s)## Quick Start(.*?)### Automated Setup').Groups[1].Value
    if (-not $quickStart) { throw "CONTRIBUTING.md has no '## Quick Start' section before '### Automated Setup'." }
    # Bash-only venv lines in Quick Start, with nothing there for Windows, strand a Windows newcomer.
    $bashOnly = $quickStart -match 'python3\.14 -m venv|source \.venv/bin/activate'
    $windowsLines = $quickStart -match 'Scripts\\Activate|py -3\.14|setup-dev\.ps1|automated-setup-windows'
    if ($bashOnly -and -not $windowsLines) {
        # Run it as written in the default Windows terminal, to show what a newcomer sees.
        $code = Invoke-Shell -Shell powershell -LogName '04-quickstart-venv' -TimeoutMinutes 5 -Command 'python3.14 -m venv .venv'
        Add-Finding "Quick Start's commands are bash only (python3.14, source .venv/bin/activate): 'python3.14 -m venv .venv' in Windows PowerShell exits $code. It also runs before any setup step has installed Python. A Windows newcomer has to skip to Automated Setup (Windows)."
    }
}

# --- 5. PowerShell 7 -------------------------------------------------------------------------
Invoke-Step 'Install PowerShell 7 (pwsh)' {
    Assert-DocText 'CONTRIBUTING.md' 'pwsh tools/setup-dev.ps1'
    if (Get-Command pwsh -ErrorAction SilentlyContinue) { return 'SKIP: pwsh is already installed' }
    $documented = Test-DocMatch 'CONTRIBUTING.md' 'winget install[^\r\n`]*Microsoft\.PowerShell'
    if (-not $documented) {
        Add-Finding 'Automated Setup (Windows) runs pwsh, which a clean Windows does not have, and never says to install PowerShell 7 (installed here with winget install Microsoft.PowerShell).'
    }
    Install-WithWinGet -Id 'Microsoft.PowerShell' -LogName '05-install-pwsh'
    Add-Note "pwsh $(& pwsh -NoProfile -Command '$PSVersionTable.PSVersion.ToString()')"
}

# --- 6. Automated Setup (Windows) ------------------------------------------------------------
Invoke-Step 'pwsh tools/setup-dev.ps1' {
    try {
        $code = Invoke-Shell -Shell pwsh -LogName '06-setup-dev' -TimeoutMinutes $SetupTimeoutMinutes -Command 'pwsh tools/setup-dev.ps1'
    }
    catch {
        # A setup that runs this long is a finding in itself; the installers' logs are in
        # 06-setup-dev-diagnostics. Nothing after it can run without the toolchain, so stop.
        Save-Diagnostic -Prefix '06-setup-dev'
        $vsLog = Get-ChildItem -Path (Join-Path $env:TEMP 'dd_installer_elevated_*.log') -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $packages = if ($vsLog) { @(Select-String -LiteralPath $vsLog.FullName -Pattern 'BEGIN: Installing').Count } else { 0 }
        $script:Current.Findings++
        $null = $script:Current.Notes.Add("FINDING: tools/setup-dev.ps1 did not finish within $SetupTimeoutMinutes min (the Visual Studio Build Tools installer had started $packages packages); logs in 06-setup-dev-diagnostics.")
        throw "STOPRUN: $($_.Exception.Message)"
    }
    if ($code -ne 0) {
        Save-Diagnostic -Prefix '06-setup-dev'
        throw "tools/setup-dev.ps1 exited $code`n$(Get-LogTail '06-setup-dev')"
    }
    # Its last lines ask for a new terminal; a newcomer opens one.
    Update-SessionEnvironment
    Add-Note "LLVM_ROOT=$env:LLVM_ROOT"
}

Invoke-Step '.venv\Scripts\Activate.ps1' {
    Assert-DocText 'CONTRIBUTING.md' '.venv\Scripts\Activate.ps1'
    $code = Invoke-Shell -Shell pwsh -LogName '07-activate-pwsh' -TimeoutMinutes 2 -Command '.venv\Scripts\Activate.ps1; if (-not $env:VIRTUAL_ENV) { exit 1 }; python --version'
    if ($code -ne 0) { throw "Activate.ps1 failed in pwsh ($code)`n$(Get-LogTail '07-activate-pwsh')" }
    # The same line in Windows PowerShell, the terminal a clean Windows opens.
    $code = Invoke-Shell -Shell powershell -LogName '07-activate-windows-powershell' -TimeoutMinutes 2 -Command '.venv\Scripts\Activate.ps1; if (-not $env:VIRTUAL_ENV) { exit 1 }'
    if ($code -ne 0) {
        # Does Automated Setup (Windows) say to use a PowerShell 7 terminal?
        $saysPwshTerminal = Test-DocMatch 'CONTRIBUTING.md' '(?s)### Automated Setup \(Windows\)(?:(?!\n#{2,3} ).)*PowerShell 7\s+terminal'
        if (-not $saysPwshTerminal) {
            Add-Finding "'.venv\Scripts\Activate.ps1' fails in Windows PowerShell, the default terminal (execution policy $(& powershell.exe -NoProfile -Command Get-ExecutionPolicy)); the docs don't say to run the setup lines in a PowerShell 7 (pwsh) terminal, where it works."
        }
    }
}

Invoke-Step 'pwsh tools/check-prereqs.ps1' {
    Assert-DocText 'CONTRIBUTING.md' 'pwsh tools/check-prereqs.ps1'
    $code = Invoke-Shell -Shell pwsh -LogName '08-check-prereqs' -TimeoutMinutes 5 -Command '.venv\Scripts\Activate.ps1; pwsh tools/check-prereqs.ps1'
    $failRows = @(Get-Content -LiteralPath (Join-Path $OutDir '08-check-prereqs.log') -Encoding utf8 |
            Where-Object { $_ -match '^\s*\u2717' } | ForEach-Object { $_.Trim() -replace '\s+', ' ' })
    if ($failRows) { Add-Note "rows marked as failing: $($failRows -join '; ')" }
    if ($code -ne 0) {
        Add-Finding "check-prereqs.ps1 exits $code right after setup-dev.ps1 in a new terminal: setup leaves a required prerequisite missing."
    }
}

# The pre-commit hooks. Where the docs make the developer tools an opt-in "before your first pull
# request" step (setup-dev.ps1 -DevTools), it runs after the build and launch, as they order it.
$DevToolsOptIn = Test-DocText 'CONTRIBUTING.md' 'setup-dev.ps1 -DevTools'
$PreCommitStep = {
    Assert-DocText 'CONTRIBUTING.md' 'pre-commit install'
    $command = '.venv\Scripts\Activate.ps1; pre-commit install'
    if ($DevToolsOptIn) { $command = ".venv\Scripts\Activate.ps1; pwsh tools/setup-dev.ps1 -DevTools; if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; pre-commit install" }
    $code = Invoke-Shell -Shell pwsh -LogName '09-pre-commit-install' -TimeoutMinutes 30 -Command $command
    if ($code -ne 0) { Add-Finding "'$command' exits $code`n$(Get-LogTail '09-pre-commit-install' 5)" }
}
if (-not $DevToolsOptIn) { Invoke-Step 'pre-commit install (Quick Start, recommended)' $PreCommitStep }

# --- 7. Build and test -----------------------------------------------------------------------
Invoke-Step 'cmake --workflow --preset win-dev (configure, build, ctest)' {
    Assert-DocText 'CONTRIBUTING.md' 'cmake --workflow --preset win-dev'
    $code = Invoke-Shell -Shell pwsh -LogName '10-win-dev' -TimeoutMinutes 240 -Command '.venv\Scripts\Activate.ps1; cmake --workflow --preset win-dev'
    $summary = Select-String -LiteralPath (Join-Path $OutDir '10-win-dev.log') -Pattern 'tests passed, \d+ tests failed out of' | Select-Object -Last 1
    if ($summary) { Add-Note $summary.Line.Trim() }
    if ($code -ne 0) {
        $failedTests = @(Select-String -LiteralPath (Join-Path $OutDir '10-win-dev.log') -Pattern '^\s*\d+ - (\S+) \(Failed\)' |
                ForEach-Object { $_.Matches[0].Groups[1].Value })
        $built = Test-Path -LiteralPath (Join-Path $RepoDir 'build\win-debug\bin\TaskSmack.exe')
        if ($built -and $failedTests) {
            # The build finished and only tests failed: record it and go on to the launch and the
            # benchmarks, which don't depend on them.
            throw "NOSTOP: ctest failed: $($failedTests -join ', ')"
        }
        throw "cmake --workflow --preset win-dev exited $code`n$(Get-LogTail '10-win-dev' 30)"
    }
}

# What the build used, and what setup installed: evidence for #2618's install inventory.
Invoke-Step -Setup 'Toolchain evidence (what the build used)' {
  # Evidence only: a failure here is noted, never stops the run. clang -v writes to stderr, which
  # Windows PowerShell turns into errors under 'Stop'.
  $ErrorActionPreference = 'Continue'
  try {
    $cache =Join-Path $RepoDir 'build\win-debug\CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        $keys = 'CMAKE_COMMAND', 'CMAKE_MAKE_PROGRAM', 'CMAKE_CXX_COMPILER', 'CMAKE_LINKER', 'CMAKE_RC_COMPILER', 'CMAKE_MT', 'Python3_EXECUTABLE', 'GIT_EXECUTABLE', 'CCACHE_PROGRAM'
        $lines = Get-Content -LiteralPath $cache | Where-Object { $_ -match "^($($keys -join '|')):" }
        Add-Note "CMakeCache: $($lines -join '; ')"
    }
    $clang = Join-Path $env:LLVM_ROOT 'bin\clang++.exe'
    if (Test-Path -LiteralPath $clang) {
        $probe = Join-Path $env:TEMP 'newcomer-probe.cpp'
        Set-Content -LiteralPath $probe -Value "#include <vector>`n#include <windows.h>`nint main() { return static_cast<int>(std::vector<int>{}.size()) + (GetCurrentProcessId() == 0); }" -Encoding ascii
        $verbose = (& $clang -v -fsyntax-only $probe 2>&1) | Out-String
        $includes = [regex]::Match($verbose, '(?s)search starts here:(.*?)End of search').Groups[1].Value -split "`r?`n" |
            ForEach-Object { $_.Trim() } | Where-Object { $_ }
        Add-Note "clang include dirs: $($includes -join '; ')"
        $link = (& $clang -### $probe 2>&1) | Out-String
        $libPaths = [regex]::Matches($link, '-libpath:([^"]+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
        Add-Note "clang link libpaths: $($libPaths -join '; ')"
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $json = & $vswhere -products * -include packages -format json | Out-String | ConvertFrom-Json
        foreach ($vs in @($json)) {
            $components = @($vs.packages | Where-Object { $_.type -eq 'Component' } | ForEach-Object { $_.id })
            Add-Note "$($vs.displayName) $($vs.installationVersion): $(@($vs.packages).Count) packages; components: $($components -join ', ')"
            $size = (Get-ChildItem -LiteralPath $vs.installationPath -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
            Add-Note ("{0}: {1:N1} GB" -f $vs.installationPath, ($size / 1GB))
        }
    }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    if (Test-Path -LiteralPath $kits) {
        $size = (Get-ChildItem -LiteralPath $kits -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        Add-Note ("{0}: {1:N1} GB" -f $kits, ($size / 1GB))
    }
    # The Visual Studio installer's own record: packages installed and how long it took.
    $vsLog = Get-ChildItem -Path (Join-Path $env:TEMP 'dd_installer_elevated_*.log') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -First 1
    if ($vsLog) {
        $stamps = Select-String -LiteralPath $vsLog.FullName -Pattern '^\[[0-9a-f]+:[0-9a-f]+\]\[(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)\]' |
            ForEach-Object { [datetime]$_.Matches[0].Groups[1].Value }
        $installs = @(Select-String -LiteralPath $vsLog.FullName -Pattern 'BEGIN: Installing').Count
        $msis = @(Select-String -LiteralPath $vsLog.FullName -Pattern "\.msi' succeeded" | ForEach-Object { $_.Line -replace ".*[\\/ ]([^\\/]+\.msi)' succeeded.*", '$1' } | Sort-Object -Unique).Count
        if ($stamps) { Add-Note ("VS installer: {0} packages installed, {1} MSIs downloaded, {2:N0} min" -f $installs, $msis, ($stamps[-1] - $stamps[0]).TotalMinutes) }
    }
    $llvmSize = (Get-ChildItem -LiteralPath $env:LLVM_ROOT -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    Add-Note ("{0}: {1:N1} GB" -f $env:LLVM_ROOT, ($llvmSize / 1GB))
  }
  catch { Add-Note "evidence incomplete: $($_.Exception.Message)" }
}

# --- 8. Launch TaskSmack once ----------------------------------------------------------------
if ($WindowsSandbox) {
    Invoke-Step -Setup 'Machine: Mesa llvmpipe beside TaskSmack.exe (no GPU in the sandbox)' {
        # A sandbox without vGPU has only Windows' OpenGL 1.1, below TaskSmack's 3.3 (user guide,
        # System requirements), and a newcomer's PC has a GPU driver. So, as CI does for its GPU-less
        # runners (.github/actions/setup-mesa-llvmpipe, the same pinned archive and SHA-256), put
        # Mesa's software OpenGL beside the exe. Not a newcomer step.
        $url = 'https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z'
        $sha256 = '351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695'
        $mesaRoot = Join-Path $env:TEMP 'newcomer-mesa'
        $null = New-Item -ItemType Directory -Force -Path $mesaRoot
        $archive = Join-Path $mesaRoot 'mesa.7z'
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
        $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $sha256) { throw "Mesa archive SHA-256 mismatch: expected $sha256, got $actual" }
        & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $mesaRoot 'x64/opengl32.dll' 'x64/libgallium_wgl.dll'
        if ($LASTEXITCODE -ne 0) { throw "extracting Mesa failed (tar exit $LASTEXITCODE)" }
        $bin = Join-Path $RepoDir 'build\win-debug\bin'
        Copy-Item -Path (Join-Path $mesaRoot 'x64\*.dll') -Destination $bin
        $env:GALLIUM_DRIVER = 'llvmpipe'
        Add-Note "Mesa 26.2.4 llvmpipe (opengl32.dll, libgallium_wgl.dll) copied to $bin; GALLIUM_DRIVER=llvmpipe"
    }
}

Invoke-Step 'Launch TaskSmack once (smoke test, no input)' {
    Assert-DocText 'CONTRIBUTING.md' 'build/win-debug/bin/TaskSmack.exe'
    Assert-DocText 'CONTRIBUTING.md' 'TASKSMACK_CONFIG_DIR'
    $exe = Join-Path $RepoDir 'build\win-debug\bin\TaskSmack.exe'
    $configDir = Join-Path $env:TEMP "tasksmack-newcomer-$PID"
    $env:TASKSMACK_CONFIG_DIR = $configDir
    $debugLog = Join-Path ([IO.Path]::GetTempPath()) 'tasksmack-debug.log'
    try {
        $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
        $null = $p.Handle
        $deadline = (Get-Date).AddSeconds(60)
        while ($p.MainWindowHandle -eq [IntPtr]::Zero -and -not $p.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $p.Refresh()
        }
        if ($p.HasExited) { throw "TaskSmack exited during startup with code $($p.ExitCode)" }
        if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw 'TaskSmack showed no main window within 60 s' }
        Start-Sleep -Seconds 10  # let it sample and render for a while
        $p.Refresh()
        if ($p.HasExited) { throw "TaskSmack exited after its window appeared, with code $($p.ExitCode)" }
        Add-Note "window '$($p.MainWindowTitle)' up; $([math]::Round($p.WorkingSet64 / 1MB)) MB working set after 10 s"
        Save-WindowCapture -Handle $p.MainWindowHandle -Path (Join-Path $OutDir 'smoke-taskSmack.png')
        if (-not $p.CloseMainWindow()) { throw 'TaskSmack did not accept a close request' }
        if (-not $p.WaitForExit(30000)) {
            Stop-Process -Id $p.Id -Force
            throw 'TaskSmack did not exit within 30 s of a close request'
        }
        if ($p.ExitCode -ne 0) { throw "TaskSmack exited with code $($p.ExitCode) after a close request" }
        Add-Note 'closed cleanly (exit 0)'
    }
    finally {
        if (Test-Path -LiteralPath $debugLog) { Copy-Item -LiteralPath $debugLog -Destination (Join-Path $OutDir 'smoke-tasksmack-debug.log') }
        Remove-Item Env:TASKSMACK_CONFIG_DIR
        Remove-Item -Recurse -Force -LiteralPath $configDir -ErrorAction SilentlyContinue
    }
}

if ($DevToolsOptIn) { Invoke-Step 'Before the first pull request: setup-dev.ps1 -DevTools + pre-commit install' $PreCommitStep }

# --- 9. Benchmarks and profiling -------------------------------------------------------------
Invoke-Step 'Benchmarks: win-benchmark build + pwsh tools/bench.ps1 win-benchmark' {
    if ($SkipPerf) { return 'SKIP: -SkipPerf' }
    Assert-DocText 'CONTRIBUTING.md' 'cmake --preset win-benchmark'
    Assert-DocText 'CONTRIBUTING.md' 'cmake --build --preset win-benchmark'
    Assert-DocText 'CONTRIBUTING.md' 'pwsh tools/bench.ps1 win-benchmark'
    $code = Invoke-Shell -Shell pwsh -LogName '12-benchmark-build' -TimeoutMinutes 180 -Command @'
.venv\Scripts\Activate.ps1
cmake --preset win-benchmark
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset win-benchmark
'@
    if ($code -ne 0) { throw "win-benchmark configure/build exited $code`n$(Get-LogTail '12-benchmark-build' 30)" }
    $code = Invoke-Shell -Shell pwsh -LogName '12-bench' -TimeoutMinutes 90 -Command '.venv\Scripts\Activate.ps1; pwsh tools/bench.ps1 win-benchmark'
    $result = Select-String -LiteralPath (Join-Path $OutDir '12-bench.log') -Pattern '^RESULT=(.+)$' | Select-Object -Last 1
    if ($code -ne 0 -or -not $result) { throw "tools/bench.ps1 exited $code without a RESULT line`n$(Get-LogTail '12-bench' 30)" }
    Add-Note "RESULT=$(Split-Path -Leaf $result.Matches[0].Groups[1].Value)"
}

Invoke-Step 'Profiling: pwsh tools/profile-etw.ps1 bench + analyze-etw.ps1' {
    if ($SkipPerf) { return 'SKIP: -SkipPerf' }
    $docCommand = 'pwsh tools/profile-etw.ps1 bench -BenchmarkFilter ''BM_ProcessModel_Refresh$'''
    Assert-DocText 'CONTRIBUTING.md' $docCommand
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    $command = $docCommand
    if ($isAdmin) {
        # profile-etw.ps1 refuses an elevated terminal unless -ElevatedTarget, and the docs give that
        # option; Windows Sandbox's only terminal is elevated.
        $command += ' -ElevatedTarget'
        Add-Note 'terminal is elevated, so -ElevatedTarget (documented) was added'
    }
    $code = Invoke-Shell -Shell pwsh -LogName '13-profile-etw' -TimeoutMinutes 30 -Command ".venv\Scripts\Activate.ps1; $command"
    $trace = Select-String -LiteralPath (Join-Path $OutDir '13-profile-etw.log') -Pattern '^TRACE=(.+)$' | Select-Object -Last 1
    if ($code -ne 0 -or -not $trace) {
        Add-Finding "profile-etw.ps1 bench exited $code without a TRACE line: $(Get-LogTail '13-profile-etw' 8)"
        return
    }
    $tracePath = $trace.Matches[0].Groups[1].Value.Trim()
    Add-Note "TRACE=$(Split-Path -Leaf $tracePath)"
    Assert-DocText 'CONTRIBUTING.md' 'pwsh tools/analyze-etw.ps1 -TracePath'
    # analyze-etw.ps1 needs xperf, from the Windows Performance Toolkit. Where the docs say setup-dev.ps1
    # doesn't install it and how to get it, its absence is expected, not a finding.
    # \s+, not a space: CONTRIBUTING.md wraps this sentence (#2676).
    $wptOptional = Test-DocMatch 'CONTRIBUTING.md' 'Windows\s+Performance\s+Toolkit,\s+which\s+`tools/setup-dev\.ps1`\s+does\s+not\s+install'
    if ($wptOptional -and -not (Get-Command xperf -ErrorAction SilentlyContinue)) {
        return 'SKIP: trace recorded; analyze-etw.ps1 not run: xperf (Windows Performance Toolkit) is an optional install the docs describe'
    }
    $code = Invoke-Shell -Shell pwsh -LogName '13-analyze-etw' -TimeoutMinutes 30 -Command ".venv\Scripts\Activate.ps1; pwsh tools/analyze-etw.ps1 -TracePath '$tracePath' -SkipFunctions"
    if ($code -ne 0) {
        Add-Finding "analyze-etw.ps1 exited $code on that trace: $(Get-LogTail '13-analyze-etw' 8)"
    }
}

# --- Done -----------------------------------------------------------------------------------
$failed = @($script:Results | Where-Object { $_.Status -eq 'FAIL' }).Count
$findings = @($script:Results | Where-Object { $_.Status -eq 'FINDING' }).Count
$overall = if ($failed) { 'FAIL' } elseif ($findings) { 'FINDINGS' } else { 'PASS' }
$total = [TimeSpan]::FromSeconds([math]::Round(((Get-Date) - $script:RunStart).TotalSeconds))
Write-CheckLog "Overall: $overall ($findings step(s) with findings, $failed failed) in $total; commit $script:Commit"
Write-Result
Write-OutFile -Name 'DONE' -Text "$overall $script:Commit $total`r`n"
if ($ShutdownWhenDone) {
    & shutdown.exe /s /t 15 /c 'newcomer-check finished'
}
exit $(if ($overall -eq 'PASS') { 0 } elseif ($overall -eq 'FINDINGS') { 2 } else { 1 })
