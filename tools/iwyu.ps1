<#
.SYNOPSIS
    Run include-what-you-use (IWYU) from Windows, through tools/iwyu.sh in WSL.
.DESCRIPTION
    IWYU runs against the Linux build tree (build/debug or build/relwithdebinfo, from the Linux
    CMake presets), so this wrapper runs tools/iwyu.sh inside WSL. Configure and build that tree in
    WSL first: cmake --preset debug && cmake --build build/debug.
    Flags map onto iwyu.sh's: -VerboseOutput (-v), -Jobs (-j), -Fix (-f), -Report (-r).
.PARAMETER BuildType
    Build type to use: debug, relwithdebinfo (default: debug)
.PARAMETER VerboseOutput
    Show verbose output with per-file progress
.PARAMETER Jobs
    Number of parallel jobs (default: iwyu.sh's, the CPU count)
.PARAMETER Fix
    Apply suggested fixes with fix_includes.py
.PARAMETER Report
    Report only; don't fail on issues
.PARAMETER Files
    Specific files to analyze, relative to the repository root (default: all of src/)
.EXAMPLE
    .\tools\iwyu.ps1 -Report
.EXAMPLE
    .\tools\iwyu.ps1 debug src\Domain\ProcessModel.cpp
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('debug', 'relwithdebinfo')]
    [string]$BuildType = 'debug',

    [Parameter()]
    [switch]$VerboseOutput,

    [Parameter()]
    [Nullable[int]]$Jobs,

    [Parameter()]
    [switch]$Fix,

    [Parameter()]
    [switch]$Report,

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path $scriptDir 'iwyu.sh'
$projectRoot = Split-Path -Parent $scriptDir

if (-not (Test-Path $scriptPath)) {
    Write-Error "Missing script: $scriptPath"
    exit 1
}

# WSL, not Git Bash: iwyu.sh reads build/<type>/compile_commands.json, which only the Linux presets
# produce, and its Linux compiler flags and /mnt/... paths need the Linux IWYU.
$wsl = Get-Command wsl.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $wsl) {
    Write-Error 'wsl.exe was not found. IWYU runs against the Linux build tree; install WSL (see CONTRIBUTING.md).'
    exit 1
}

# The repository root as WSL sees it (honours a custom automount root, unlike a hard-coded /mnt/<drive>).
$projectRootForBash = (& $wsl.Source -e wslpath -a ($projectRoot -replace '\\', '/') | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or -not $projectRootForBash) {
    Write-Error "wslpath could not translate '$projectRoot' into a WSL path."
    exit 1
}

$iwyuArgs = @()
if ($VerboseOutput) { $iwyuArgs += '-v' }
if ($null -ne $Jobs -and $Jobs -gt 0) {
    $iwyuArgs += '-j'
    $iwyuArgs += "$Jobs"
}
if ($Fix) { $iwyuArgs += '-f' }
if ($Report) { $iwyuArgs += '-r' }
$iwyuArgs += $BuildType
# iwyu.sh resolves relative paths against the repository root; give it forward slashes.
if ($Files) { $iwyuArgs += $Files | ForEach-Object { $_ -replace '\\', '/' } }

# Single-quote each argument for bash; an embedded ' becomes '\'' (close, escaped quote, reopen).
$quotedRoot = "'" + $projectRootForBash.Replace("'", "'\''") + "'"
$commandLine = "cd $quotedRoot && ./tools/iwyu.sh"
foreach ($arg in $iwyuArgs) {
    $commandLine += " '" + $arg.Replace("'", "'\''") + "'"
}

& $wsl.Source -e bash -lc $commandLine
exit $LASTEXITCODE
