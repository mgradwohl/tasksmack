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

$gitBashPath = 'C:/Program Files/Git/bin/bash.exe'
if (Test-Path $gitBashPath) {
    $bashPath = $gitBashPath
    $useGitBash = $true
}
else {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $bash) {
        Write-Error 'bash was not found in PATH. Install Git Bash/WSL and ensure bash is available.'
        exit 1
    }
    $bashPath = $bash.Source
    $useGitBash = $false
}

$scriptPathForBash = $scriptPath -replace '\\', '/'
$args = @()
if ($VerboseOutput) { $args += '-v' }
if ($null -ne $Jobs -and $Jobs -gt 0) {
    $args += '-j'
    $args += "$Jobs"
}
if ($Fix) { $args += '-f' }
if ($Report) { $args += '-r' }
$args += $BuildType
if ($Files) { $args += $Files }

if ($useGitBash) {
    & $bashPath $scriptPathForBash @args
    exit $LASTEXITCODE
}

$projectRootForBash = ($projectRoot -replace '\\', '/')
if ($projectRootForBash -match '^([A-Za-z]):/(.*)$') {
    $projectRootForBash = "/mnt/$($Matches[1].ToLower())/$($Matches[2])"
}

$commandLine = "cd '$projectRootForBash' && ./tools/iwyu.sh"
foreach ($arg in $args) {
    # Single-quote each arg to prevent word-splitting and shell metacharacter expansion.
    $escaped = $arg -replace "'", "'\"'\"'"
    $commandLine += " '$escaped'"
}

& $bashPath -lc $commandLine
exit $LASTEXITCODE
