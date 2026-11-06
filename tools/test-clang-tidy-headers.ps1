# Tests that the local clang-tidy scripts report a finding in a project header the analyzed .cpp
# includes, as CI's Windows clang-tidy (tools/clang-tidy.ps1) does (#3500). Runs
# tools/clang-tidy.ps1 -BuildDir, and tools/clang-tidy.sh --build-dir through Git Bash when Git for
# Windows is installed, on tests/tools/fixtures/clang-tidy-headers with a one-entry compile database,
# so no configured tree is needed. Registered in CTest on Windows with PowerShell 7; can also run
# directly with pwsh -File. Skips (passes) when clang-tidy isn't installed.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureDir = Join-Path $repoRoot 'tests\tools\fixtures\clang-tidy-headers'
$fixtureCpp = Join-Path $fixtureDir 'HeaderFinding.cpp'
# The finding's location: "HeaderFinding.h:<line>:<column>: warning: ... [check]"
$expected = 'HeaderFinding\.h:\d+:\d+: (warning|error): .*\[readability-math-missing-parentheses'

$clangTidy = Find-LlvmTool -Name 'clang-tidy'
if (-not $clangTidy) {
    Write-Host 'clang-tidy not found; clang-tidy header tests skipped'
    exit 0
}

$root = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-clang-tidy-headers-$([guid]::NewGuid().ToString('N'))"
$buildDir = Join-Path $root 'build'
New-Item -ItemType Directory -Path $buildDir | Out-Null
try {
    # One entry, forward slashes as CMake writes them on Windows. No standard headers: fast, and the
    # result doesn't depend on the installed SDK or STL.
    $cpp = $fixtureCpp -replace '\\', '/'
    $dir = $fixtureDir -replace '\\', '/'
    $compdb = @(@{ directory = $dir; file = $cpp; arguments = @('clang++', '-std=c++23', "-I$dir", '-c', $cpp) })
    [IO.File]::WriteAllText((Join-Path $buildDir 'compile_commands.json'), (ConvertTo-Json -InputObject $compdb -Depth 4))

    # ── tools/clang-tidy.ps1, the script CI's Windows clang-tidy runs ───────────────────────────
    $output = & pwsh -NoLogo -NoProfile -File (Join-Path $PSScriptRoot 'clang-tidy.ps1') -BuildDir $buildDir -Files $fixtureCpp -Jobs 1 *>&1 | Out-String
    $code = $LASTEXITCODE
    Assert-True ($output -match $expected) "clang-tidy.ps1 did not report the header finding:`n$output"
    Assert-True ($code -ne 0) "clang-tidy.ps1 reported the header finding but exited $code"
    Assert-True ($output -notmatch 'HeaderFinding\.cpp:\d+:\d+: (warning|error)') "the fixture .cpp has its own finding (fix the fixture):`n$output"
    Assert-True ($output -notmatch 'python not found') "clang-tidy.ps1 found no working python:`n$output"

    # ── tools/clang-tidy.sh from Git Bash, the command agents run, which hands off to clang-tidy.ps1 ─
    # Git for Windows' own bash, never a bare "bash" from PATH: on Windows that can be WSL's.
    $gitExecPath = (& git --exec-path 2>$null)
    $gitBash = if ($gitExecPath) { Join-Path $gitExecPath '..\..\..\bin\bash.exe' } else { $null }
    if ($gitBash -and (Test-Path -LiteralPath $gitBash)) {
        $gitBash = (Resolve-Path -LiteralPath $gitBash).ProviderPath
        $shScript = (Join-Path $PSScriptRoot 'clang-tidy.sh') -replace '\\', '/'
        $output = & $gitBash $shScript --build-dir ($buildDir -replace '\\', '/') -j 1 'tests/tools/fixtures/clang-tidy-headers/HeaderFinding.cpp' 2>&1 | Out-String
        $code = $LASTEXITCODE
        Assert-True ($output -match $expected) "clang-tidy.sh (Git Bash) did not report the header finding:`n$output"
        Assert-True ($code -ne 0) "clang-tidy.sh (Git Bash) reported the header finding but exited $code"
    } else {
        Write-Host 'Git Bash not found; the clang-tidy.sh hand-off was not tested'
    }

    Write-Host 'clang-tidy header tests passed'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
