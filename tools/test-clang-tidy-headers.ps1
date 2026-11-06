# Tests that the local clang-tidy scripts report a finding in a project header the analyzed .cpp
# includes, as CI's Windows clang-tidy (tools/clang-tidy.ps1) does (#3500). Runs on
# tests/tools/fixtures/clang-tidy-headers with one-entry compile databases, so no configured tree is
# needed:
#   - tools/clang-tidy.ps1 -BuildDir, with an "arguments" database and with a "command" database, each
#     carrying the PCH and module flags the script must strip (left in, they fail the compile);
#     Both run from the checkout and from a copy of the scripts whose path has a space;
#   - tools/clang-tidy.sh --build-dir through Git Bash, and with a named preset, in that copy with its
#     own CMakePresets.json (Windows Python's CRLF output and Windows paths, #3500 review).
# Registered in CTest on Windows with PowerShell 7; can also run directly with pwsh -File. Skips (passes)
# when clang-tidy isn't installed.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureRel = 'tests\tools\fixtures\clang-tidy-headers'
# The finding's location: "HeaderFinding.h:<line>:<column>: warning: ... [check]"
$expected = 'HeaderFinding\.h:\d+:\d+: (warning|error): .*\[readability-math-missing-parentheses'
$cppFinding = 'HeaderFinding\.cpp:\d+:\d+: (warning|error)'

$clangTidy = Find-LlvmTool -Name 'clang-tidy'
if (-not $clangTidy) {
    Write-Host 'clang-tidy not found; clang-tidy header tests skipped'
    exit 0
}

# A one-entry database for the fixture under $Root, in "arguments" or "command" form. The flags the
# scripts strip point at files that don't exist, so an entry they leave in fails the compile. The
# fixture .cpp #errors without TIDY_FIXTURE_DEFINE, whose value has a space (and, in the command form,
# escaped quotes), so a command split wrongly fails too. No standard headers: fast, and independent of
# the installed SDK or STL.
function Write-FixtureCompdb([string]$Root, [string]$BuildDir, [string]$Form) {
    $dir = (Join-Path $Root $fixtureRel) -replace '\\', '/'
    $cpp = "$dir/HeaderFinding.cpp"
    $unusable = @('-Xclang', '-include-pch', '-Xclang', "$dir/missing.pch",
        '-Xclang', '-include', '-Xclang', "$dir/missing/cmake_pch.hxx",
        '-Xclang', '-fno-pch-timestamp', "@$dir/missing.modmap", "-fmodule-output=$dir/missing.pcm")
    $entry = [ordered]@{ directory = $dir }
    if ($Form -eq 'arguments') {
        $entry.arguments = @('clang++', '-std=c++23', "-I$dir", '-DTIDY_FIXTURE_DEFINE="fixture name"') + $unusable + @('-c', $cpp)
    } else {
        # Every path-bearing argument quoted, as CMake does when a path has a space ($Root can: see below).
        $quoted = @("-I$dir") + $unusable + @($cpp) | ForEach-Object { if ($_ -match '[/\\]') { "`"$_`"" } else { $_ } }
        $includeArg, $rest = $quoted
        $unusableArgs = $rest[0..($rest.Count - 2)] -join ' '
        $entry.command = ('"C:\Program Files\LLVM\bin\clang++.exe" -std=c++23 {0} "-DTIDY_FIXTURE_DEFINE=\"fixture name\"" {1} -c {2}' -f
            $includeArg, $unusableArgs, $rest[-1])
    }
    $entry.file = $cpp
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $BuildDir 'compile_commands.json'), (ConvertTo-Json -InputObject @([PSCustomObject]$entry) -Depth 4))
}

function Assert-ReportsHeaderFinding([string]$What, [string]$Output, [int]$Code) {
    Assert-True ($Output -match $expected) "${What} did not report the header finding:`n$Output"
    Assert-True ($Code -ne 0) "${What} reported the header finding but exited $Code"
    Assert-True ($Output -notmatch $cppFinding) "${What}: the fixture .cpp has its own finding or failed to compile:`n$Output"
    Assert-True ($Output -notmatch 'error: .*(missing\.pch|cmake_pch|missing\.modmap|TIDY_FIXTURE_DEFINE)') "${What} kept an unusable flag or split the command wrongly:`n$Output"
    Assert-True ($Output -notmatch 'python not found') "${What} found no working python:`n$Output"
}

# Git for Windows' own bash, never a bare "bash" from PATH: on Windows that can be WSL's.
$gitExecPath = (& git --exec-path 2>$null)
$gitBash = if ($gitExecPath) { Join-Path $gitExecPath '..\..\..\bin\bash.exe' } else { $null }
$gitBash = if ($gitBash -and (Test-Path -LiteralPath $gitBash)) { (Resolve-Path -LiteralPath $gitBash).ProviderPath } else { $null }

$scratch = Join-Path ([IO.Path]::GetTempPath()) "tasksmack-clang-tidy-headers-$([guid]::NewGuid().ToString('N'))"
# A copy of the scripts and the fixture, in a directory whose name has a space: the cases below run
# from it as well as from the checkout, and the named-preset case uses its own CMakePresets.json. It
# lives in the checkout's (git-ignored) .cache, so its path is spelled like the checkout's, never a
# temp directory's 8.3 short name.
$copy = Join-Path $repoRoot ".cache\clang-tidy headers test $([guid]::NewGuid().ToString('N'))"
try {
    New-Item -ItemType Directory -Path (Join-Path $copy 'tools') -Force | Out-Null
    foreach ($file in 'clang-tidy.sh', 'clang-tidy.ps1', 'common.sh', 'common.ps1', 'dedupe-compile-commands.py') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $copy 'tools')
    }
    foreach ($file in '.clang-tidy', '.pre-commit-config.yaml') {
        Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $copy
    }
    New-Item -ItemType Directory -Path (Join-Path $copy $fixtureRel) -Force | Out-Null
    Copy-Item -Path (Join-Path $repoRoot "$fixtureRel\*") -Destination (Join-Path $copy $fixtureRel)

    # ── tools/clang-tidy.ps1, the script CI's Windows clang-tidy runs: both database forms ──────
    foreach ($root in $repoRoot, $copy) {
        foreach ($form in 'arguments', 'command') {
            $buildDir = Join-Path $scratch "$form-$([array]::IndexOf(@($repoRoot, $copy), $root))"
            Write-FixtureCompdb -Root $root -BuildDir $buildDir -Form $form
            $output = & pwsh -NoLogo -NoProfile -File (Join-Path $root 'tools\clang-tidy.ps1') -BuildDir $buildDir `
                -Files (Join-Path $root "$fixtureRel\HeaderFinding.cpp") -Jobs 1 *>&1 | Out-String
            Assert-ReportsHeaderFinding "clang-tidy.ps1 -BuildDir ($form database, from $root)" $output $LASTEXITCODE
        }
    }

    if (-not $gitBash) {
        Write-Host 'Git Bash not found; the clang-tidy.sh hand-off was not tested'
        Write-Host 'clang-tidy header tests passed'
        exit 0
    }

    # ── tools/clang-tidy.sh from Git Bash, the command agents run, which hands off to clang-tidy.ps1 ─
    $buildDir = Join-Path $scratch 'arguments-0'
    $shScript = (Join-Path $PSScriptRoot 'clang-tidy.sh') -replace '\\', '/'
    $output = & $gitBash $shScript --build-dir ($buildDir -replace '\\', '/') -j 1 "$($fixtureRel -replace '\\', '/')/HeaderFinding.cpp" 2>&1 | Out-String
    Assert-ReportsHeaderFinding 'clang-tidy.sh --build-dir (Git Bash)' $output $LASTEXITCODE

    # ── clang-tidy.sh with a named preset, from the copy: its binaryDir comes from Windows Python ─
    [IO.File]::WriteAllText((Join-Path $copy 'CMakePresets.json'), (ConvertTo-Json -Depth 4 -InputObject ([ordered]@{
                    version = 6
                    configurePresets = @(@{ name = 'tidy-fixture'; binaryDir = '${sourceDir}/build/tidy-fixture' })
                })))
    Write-FixtureCompdb -Root $copy -BuildDir (Join-Path $copy 'build\tidy-fixture') -Form 'arguments'
    $copySh = (Join-Path $copy 'tools\clang-tidy.sh') -replace '\\', '/'
    $output = & $gitBash $copySh tidy-fixture -j 1 "$($fixtureRel -replace '\\', '/')/HeaderFinding.cpp" 2>&1 | Out-String
    Assert-True ($output -notmatch 'no such directory') "clang-tidy.sh tidy-fixture did not find the preset's binaryDir:`n$output"
    Assert-ReportsHeaderFinding 'clang-tidy.sh tidy-fixture (named preset, Git Bash)' $output $LASTEXITCODE

    Write-Host 'clang-tidy header tests passed'
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $copy -Recurse -Force -ErrorAction SilentlyContinue
}
