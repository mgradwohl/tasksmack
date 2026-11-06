# Tests that the local clang-tidy scripts report a finding in a project header the analyzed .cpp
# includes, as CI's Windows clang-tidy (tools/clang-tidy.ps1) does (#3500). Runs on
# tests/tools/fixtures/clang-tidy-headers with one-entry compile databases, so no configured tree is
# needed:
#   - tools/clang-tidy.ps1 -BuildDir, with an "arguments" database and with a "command" database, each
#     carrying the PCH and module flags the script must strip (left in, they fail the compile);
#     Both run from the checkout and from a copy of the scripts whose path has a space;
#   - tools/clang-tidy.sh --build-dir through Git Bash, and with a named preset, in that copy with its
#     own CMakePresets.json (Windows Python's CRLF output and Windows paths, #3500 review), also with
#     a stand-in Python whose CR survives $(...), as under MSYS2 and Cygwin (#3517);
#   - a malformed database fails both scripts and leaves no temporary directory in TEMP (#3517);
#   - clang-tidy.ps1's Python lookup isn't misled by an earlier command's exit code (#3523).
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

# ── clang-tidy.ps1's Find-WorkingPython reads each Python check's own exit code (#3523) ─────────
# A stale non-zero $LASTEXITCODE from an earlier command must not make a working Python look broken.
# The function is loaded from the script's AST, since running the script needs a build tree.
$scriptAst = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'clang-tidy.ps1'), [ref]$null, [ref]$null)
$findPythonAst = $scriptAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Find-WorkingPython'
    }, $true)
Assert-True ($null -ne $findPythonAst) 'clang-tidy.ps1 has no Find-WorkingPython function'
. ([scriptblock]::Create($findPythonAst.Extent.Text))
$ProjectRoot = $repoRoot
$global:LASTEXITCODE = 0
$freshPython = Find-WorkingPython
if ($freshPython) {
    $global:LASTEXITCODE = 7
    $stalePython = Find-WorkingPython
    Assert-True ($stalePython -eq $freshPython) "Find-WorkingPython after a stale exit code 7 chose '$stalePython', not '$freshPython'"
} else {
    Write-Host 'No working python; the stale exit code case was not tested'
}

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

    # ── The same preset under a bash that keeps native Python's CR (MSYS2, Cygwin; #3517) ─────────
    # Git Bash's $(...) drops a CRLF's CR, so the copy's venv gets a stand-in for native Windows Python
    # that ends its sys.platform and binaryDir lines with "\r \r\n", whose first CR survives, and that
    # passes paths through unconverted, as Cygwin does. Read as "win32\r ", the platform makes the
    # lookup hand the POSIX root to a native Python, which can't open it. The version probe answers a
    # fixed supported 3.14, so find_python picks the stand-in whatever the backing Python's version
    # (an older one would be rejected, and the fallback would run the real Python without any CR).
    # The stand-in logs its other calls, and the case asserts it ran the platform probe.
    $realPython = foreach ($name in 'python', 'python3') {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $command) { continue }
        # Not "| Select-Object -First 1": stopping the pipeline early leaves $LASTEXITCODE unset. And
        # not indexed before the count is checked: the Store's python alias can print nothing, and
        # [0] of an empty array throws under strict mode.
        try {
            $lines = @(& $command.Source -I -c 'import sys; print(sys.executable)' 2>$null)
        } catch {
            continue
        }
        if ($LASTEXITCODE -ne 0 -or $lines.Count -eq 0) { continue }
        $exe = $lines[0]
        if ($exe -and (Test-Path -LiteralPath $exe)) { $exe; break }
    }
    if ($realPython) {
        $shim = Join-Path $copy '.venv\bin\python'
        $shimLog = Join-Path $scratch 'stand-in-python.log'
        New-Item -ItemType Directory -Path (Split-Path -Parent $shim) -Force | Out-Null
        New-Item -ItemType Directory -Path $scratch -Force | Out-Null
        $shimText = @(
            '#!/usr/bin/env bash'
            "real='$($realPython -replace '\\', '/')'"
            "log='$($shimLog -replace '\\', '/')'"
            'export MSYS_NO_PATHCONV=1'
            'case "$*" in *sys.version_info*) echo 3.14; exit 0 ;; esac'
            'printf ''%s\n'' "$*" >> "$log"'
            'out="$("$real" "$@")" || exit'
            'printf ''%s\r \r\n'' "$out"'
        ) -join "`n"
        [IO.File]::WriteAllText($shim, "$shimText`n")
        $output = & $gitBash $copySh tidy-fixture -j 1 "$($fixtureRel -replace '\\', '/')/HeaderFinding.cpp" 2>&1 | Out-String
        $code = $LASTEXITCODE
        Remove-Item -LiteralPath (Join-Path $copy '.venv') -Recurse -Force
        $shimCalls = if (Test-Path -LiteralPath $shimLog) { Get-Content -LiteralPath $shimLog -Raw } else { '' }
        Assert-True ($shimCalls -match 'sys\.platform') "clang-tidy.sh tidy-fixture did not choose the CRLF stand-in Python (its calls: '$shimCalls'):`n$output"
        Assert-True ($output -notmatch 'Unknown argument|no such directory') "clang-tidy.sh tidy-fixture misread a CRLF platform or binaryDir:`n$output"
        Assert-ReportsHeaderFinding 'clang-tidy.sh tidy-fixture (CRLF Python output)' $output $code
    } else {
        Write-Host 'No python on PATH; the CRLF platform case was not tested'
    }

    # ── The hand-off fails closed (#3500 review): clang-tidy.ps1 stopping on an error, or not there ─
    $badDir = Join-Path $scratch 'malformed'
    New-Item -ItemType Directory -Path $badDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $badDir 'compile_commands.json'), '[{ "directory": ')
    # Both runs get their own TEMP, which must be empty afterwards: the sanitized database's temporary
    # directory is removed however clang-tidy.ps1 ends, a terminating error included (#3517).
    $privateTemp = Join-Path $scratch 'temp'
    New-Item -ItemType Directory -Path $privateTemp | Out-Null
    $savedTemp, $savedTmp = $env:TEMP, $env:TMP
    try {
        $env:TEMP = $env:TMP = $privateTemp
        $output = & pwsh -NoLogo -NoProfile -File (Join-Path $copy 'tools\clang-tidy.ps1') -BuildDir $badDir `
            -Files (Join-Path $copy "$fixtureRel\HeaderFinding.cpp") -Jobs 1 *>&1 | Out-String
        $code = $LASTEXITCODE
        $left = @(Get-ChildItem -LiteralPath $privateTemp -Filter '*clang-tidy*' -Force)
        Assert-True ($code -ne 0) "clang-tidy.ps1 passed (exit $code) on a malformed database:`n$output"
        Assert-True ($left.Count -eq 0) "clang-tidy.ps1 left $(($left | Select-Object -ExpandProperty Name) -join ', ') in TEMP after a malformed database:`n$output"
        $output = & $gitBash $copySh --build-dir ($badDir -replace '\\', '/') -j 1 "$($fixtureRel -replace '\\', '/')/HeaderFinding.cpp" 2>&1 | Out-String
        $code = $LASTEXITCODE
        $left = @(Get-ChildItem -LiteralPath $privateTemp -Filter '*clang-tidy*' -Force)
    } finally {
        $env:TEMP, $env:TMP = $savedTemp, $savedTmp
    }
    Assert-True ($code -ne 0 -and $output -notmatch 'completed successfully') "clang-tidy.sh passed (exit $code) although clang-tidy.ps1 failed on a malformed database:`n$output"
    Assert-True ($left.Count -eq 0) "clang-tidy.sh's hand-off left $(($left | Select-Object -ExpandProperty Name) -join ', ') in TEMP after a malformed database:`n$output"

    # No usable Python: the preset lookup says so and fails, rather than passing or guessing. PATH keeps
    # only pwsh (Git Bash adds its own tools, which have no Python); the copy has no .venv.
    $savedPath = $env:PATH
    try {
        $env:PATH = "$(Split-Path -Parent (Get-Command pwsh).Source);$env:SystemRoot\System32"
        $output = & $gitBash $copySh tidy-fixture -j 1 "$($fixtureRel -replace '\\', '/')/HeaderFinding.cpp" 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally {
        $env:PATH = $savedPath
    }
    Assert-True ($code -ne 0 -and $output -match 'needs python') "clang-tidy.sh tidy-fixture without Python: exit $code, expected a 'needs python' error:`n$output"

    Remove-Item -LiteralPath (Join-Path $copy 'tools\clang-tidy.ps1')
    $output = & $gitBash $copySh tidy-fixture -j 1 "$($fixtureRel -replace '\\', '/')/HeaderFinding.cpp" 2>&1 | Out-String
    $code = $LASTEXITCODE
    Assert-True ($code -ne 0) "clang-tidy.sh passed (exit $code) although clang-tidy.ps1 is missing:`n$output"

    Write-Host 'clang-tidy header tests passed'
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $copy -Recurse -Force -ErrorAction SilentlyContinue
}
