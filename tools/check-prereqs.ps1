<#
.SYNOPSIS
    Check Prerequisites Script for Windows.
.DESCRIPTION
    Displays all required tools, their paths, versions, and status. The compiler rows report the
    LLVM the win-* presets build with ($env:LLVM_ROOT\bin), and the Python rows the interpreter
    CMake uses (the project .venv when it exists).
.EXAMPLE
    pwsh tools/check-prereqs.ps1
#>
#Requires -Version 7
[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot "common.ps1")
$RepoRoot = Split-Path -Parent $PSScriptRoot

# The Python probes print sys.executable, which holds the user's name: decode their output as UTF-8 (#4320).
# The probes also run with -X utf8 (below), since Python writes a pipe in the ANSI code page otherwise.
# Without a console (a detached process) setting it throws; the default is kept then.
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try { [Console]::OutputEncoding = $OutputEncoding } catch { Write-Verbose "Console encoding unchanged: $_" }

# Required versions (minimum)
$MIN_CMAKE_VERSION = [Version]"3.29"
$MIN_CLANG_VERSION = 23
$MIN_CCACHE_VERSION = [Version]"4.9.1"
$MIN_PYTHON_VERSION = [Version]"3.14"
$MIN_GIT_VERSION = [Version]"2.30"
# The oldest MSVC Build Tools (the standard library clang compiles against): VS 2026 18.6's 14.51, which first
# shipped <flat_map> and <flat_set>, and what CI builds with (#4354). tools/setup-dev.ps1 installs VS 2026.
$MIN_MSVC_TOOLSET_VERSION = [Version]"14.51"

# The LLVM major CI builds and formats with (the clang-format rev in .pre-commit-config.yaml). The
# compilers the win-* presets use (clang, clang++) fail on any other major: the presets require the
# pinned one, and tools/setup-dev.ps1 replaces another (#3041). The other LLVM tools on another major
# still work, but can format or report clang-tidy findings differently from CI, and tools/pgo.ps1
# requires the pinned major exactly; those rows get a warning instead of a pass.
$PinnedLlvmMajor = Get-PinnedLlvmMajor

# Helper function to print status. Status is "ok", "warn" (usable, but see the note) or "fail".
function Write-Status {
    param(
        [string]$Name,
        [string]$Status,
        [string]$Version,
        [string]$Path,
        [string]$Required
    )

    $nameFormatted = $Name.PadRight(14)
    $versionFormatted = if ($Version) { $Version.PadRight(12) } else { "NOT FOUND".PadRight(12) }
    # A Required of "= N" is an exact version; any other is a minimum.
    $requiredFormatted = if ($Required -like '=*') { "($Required)".PadRight(12) } elseif ($Required) { "(>= $Required)".PadRight(12) } else { "".PadRight(12) }

    if ($Status -eq "ok") {
        Write-Host "$([char]0x2713) " -ForegroundColor Green -NoNewline
        Write-Host $nameFormatted -NoNewline
        Write-Host $versionFormatted -ForegroundColor Green -NoNewline
    }
    elseif ($Status -eq "warn") {
        Write-Host "! " -ForegroundColor Yellow -NoNewline
        Write-Host $nameFormatted -NoNewline
        Write-Host $versionFormatted -ForegroundColor Yellow -NoNewline
    }
    else {
        Write-Host "$([char]0x2717) " -ForegroundColor Red -NoNewline
        Write-Host $nameFormatted -NoNewline
        Write-Host $versionFormatted -ForegroundColor Red -NoNewline
    }

    Write-Host $requiredFormatted -ForegroundColor Cyan -NoNewline

    if ($Path) {
        Write-Host $Path -ForegroundColor Blue
    }
    else {
        Write-Host ""
    }
}

# Print the Smart App Control row (#4781): ok when off (or absent), warn in evaluation mode (it may switch itself
# to enforce), fail when it enforces (it blocks unsigned developer tools and every local build). Returns $false
# only on fail. State is 0 (off or missing), 1 (enforce) or 2 (evaluation), as Get-SmartAppControlState reads it.
function Write-SmartAppControlStatus {
    param([int]$State)
    switch ($State) {
        1 { Write-Status -Name "Smart App Ctl" -Status "fail" -Version "ENFORCED" -Required "= off"; return $false }
        2 { Write-Status -Name "Smart App Ctl" -Status "warn" -Version "EVALUATION" -Required "= off"; return $true }
        default { Write-Status -Name "Smart App Ctl" -Status "ok" -Version "off" -Required "= off"; return $true }
    }
}

# Print the RAM-per-core row (#4825): Ninja runs one compile per core (plus two), and about 1 GB per clang++ is what
# a TaskSmack translation unit needs, so less than that per logical processor ends in "LLVM ERROR: out of memory".
# Warns, never fails; the row names the cap. Returns the memory per core in GB.
function Write-MemoryPerCoreStatus {
    param([double]$MemoryBytes, [int]$Cores)
    if ($MemoryBytes -le 0 -or $Cores -le 0) { return $null }
    $memoryGb = $MemoryBytes / 1GB
    $perCore = $memoryGb / $Cores
    $version = ('{0:N1} GB/core' -f $perCore)
    if ($perCore -lt 1.0) {
        $jobs = [int][math]::Max(1, [math]::Floor($memoryGb))
        Write-Status -Name "RAM per core" -Status "warn" -Version $version -Path ('({0:N0} GB, {1} cores: before building, set $env:CMAKE_BUILD_PARALLEL_LEVEL = {2})' -f $memoryGb, $Cores, $jobs) -Required "1 GB"
    }
    else {
        Write-Status -Name "RAM per core" -Status "ok" -Version $version -Required "1 GB"
    }
    return $perCore
}

# Get tool path
function Get-ToolPath {
    param([string]$Command)
    try {
        $cmd = Get-Command $Command -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    catch {}
    return $null
}

# Run a tool and return the first capture group of Pattern in its output, or $null.
function Get-ToolVersion {
    param(
        [string]$Path,
        [string]$Pattern,
        [string[]]$Arguments = @('--version')
    )
    if (-not $Path) { return $null }
    try {
        $output = (& $Path @Arguments 2>&1) | Out-String
        if ($output -match $Pattern) {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# The win-* presets set CMAKE_C(XX)_COMPILER to $env:LLVM_ROOT/bin/clang(++).exe (CMakePresets.json,
# windows-base), and clang links with the lld-link beside it. With LLVM_ROOT set, report that tool
# and nothing else: an LLVM elsewhere on PATH is not what the build uses. Without LLVM_ROOT the
# presets can't configure (the Environment Variables section fails); report what Find-LlvmTool
# finds, so the row shows which install LLVM_ROOT should point at.
function Get-PresetLlvmToolPath {
    param([string]$Name)
    if ($env:LLVM_ROOT) {
        $candidate = Join-Path $env:LLVM_ROOT "bin\$Name.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
        return $null
    }
    return Find-LlvmTool -Name $Name
}

# What a missing preset tool's row shows: where the presets look for it.
function Get-PresetLlvmToolHint {
    param([string]$Name)
    if ($env:LLVM_ROOT) { return "(not found: $(Join-Path $env:LLVM_ROOT "bin\$Name.exe"))" }
    return "(set LLVM_ROOT to your LLVM install)"
}

# Print an LLVM tool's row: ok at the pinned major, warn at another major >= the minimum, fail below
# it or when missing. -RequirePinned (the compilers) fails another major too. Returns $false only on fail.
$script:LlvmSkewSeen = $false
$script:LlvmCompilerMajor = $null
function Write-LlvmToolStatus {
    param(
        [string]$Name,
        [string]$Path,
        [string]$Major,
        [string]$MissingHint,
        [switch]$RequirePinned
    )
    if ($RequirePinned -and $PinnedLlvmMajor -and $Major -and [int]$Major -ne $PinnedLlvmMajor) {
        Write-Status -Name $Name -Status "fail" -Version $Major -Path $Path -Required "= $PinnedLlvmMajor"
        $script:LlvmCompilerMajor = $Major
        return $false
    }
    if ($Major -and [int]$Major -ge $MIN_CLANG_VERSION) {
        if ($PinnedLlvmMajor -and [int]$Major -ne $PinnedLlvmMajor) {
            Write-Status -Name $Name -Status "warn" -Version $Major -Path $Path -Required $MIN_CLANG_VERSION
            $script:LlvmSkewSeen = $true
        }
        else {
            Write-Status -Name $Name -Status "ok" -Version $Major -Path $Path -Required $MIN_CLANG_VERSION
        }
        return $true
    }
    $shownPath = if ($Path) { $Path } else { $MissingHint }
    Write-Status -Name $Name -Status "fail" -Version $Major -Path $shownPath -Required $MIN_CLANG_VERSION
    return $false
}

# Get cmake version
function Get-CMakeVersion {
    try {
        $output = (& cmake --version 2>&1) | Out-String
        if ($output -match 'cmake version (\d+\.\d+(\.\d+)?)') {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# Get ccache version: two or three components (4.14, 4.9.1; #2350)
function Get-CcacheVersion {
    try {
        $output = (& ccache --version 2>&1) | Out-String
        if ($output -match 'ccache version (\d+\.\d+(?:\.\d+)?)') {
            return [Version]$Matches[1]
        }
    }
    catch {}
    return $null
}

# Get ninja version
function Get-NinjaVersion {
    try {
        $output = (& ninja --version 2>&1) | Out-String
        return $output.Trim()
    }
    catch {}
    return $null
}

# The newest MSVC toolset (VC\Tools\MSVC\<version>) of any Visual Studio vswhere lists, or $null. clang takes the
# MSVC standard library and CRT from it. Returns @{ Version = [Version]; Path = <toolset directory> }.
function Get-MsvcToolset {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $best = $null
    try {
        $installs = @(& $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
    }
    catch { return $null }
    foreach ($install in $installs) {
        $toolsets = Join-Path "$install".Trim() 'VC\Tools\MSVC'
        if (-not (Test-Path -LiteralPath $toolsets)) { continue }
        foreach ($dir in Get-ChildItem -LiteralPath $toolsets -Directory -ErrorAction SilentlyContinue) {
            $parsed = $null
            if ([Version]::TryParse($dir.Name, [ref]$parsed) -and (-not $best -or $parsed -gt $best.Version)) {
                $best = @{ Version = $parsed; Path = $dir.FullName }
            }
        }
    }
    return $best
}

# Find the RC compiler the same way cmake/RCCompiler.cmake does: the RC environment variable
# first (an explicit override CMake always honors, may carry the compiler plus arguments, and
# is skipped here if empty -- CMake treats an empty RC as unset too), then llvm-rc only, searched
# via LLVM_ROOT/bin, %ProgramFiles%\LLVM\bin, the hardcoded C:\Program Files\LLVM\bin, then PATH
# -- no fallback to the Windows SDK's rc.exe, PATH-found rc.exe, or llvm-windres. This project's
# Windows toolchain is LLVM/Clang, not MSVC, and the Windows SDK's own rc.exe is confirmed to
# hang indefinitely in this project's CI (issue #746); cmake/RCCompiler.cmake fails configuration
# outright rather than falling back to any of those unless RC opts in, so a check that ignored RC
# and reported one of them as "ok" (or reported a "fail" when RC selects a compiler CMake would
# accept) would disagree with what configuration actually does.
# Returns a hashtable @{ Path = <string or $null>; Source = <display label> }.
function Get-RcCompilerPath {
    if ($env:RC -and $env:RC.Trim() -ne "") {
        $rcValue = $env:RC.Trim()
        if ($rcValue.StartsWith('"')) {
            $endQuote = $rcValue.IndexOf('"', 1)
            $rcExe = if ($endQuote -gt 0) { $rcValue.Substring(1, $endQuote - 1) } else { $rcValue }
        }
        else {
            $rcExe = ($rcValue -split '\s+', 2)[0]
        }
        if (Test-Path $rcExe) {
            return @{ Path = $rcExe; Source = "RC" }
        }
    }
    if ($env:LLVM_ROOT) {
        $candidate = Join-Path $env:LLVM_ROOT "bin\llvm-rc.exe"
        if (Test-Path $candidate) {
            return @{ Path = $candidate; Source = "llvm-rc" }
        }
    }
    if ($env:ProgramFiles) {
        $candidate = Join-Path $env:ProgramFiles "LLVM\bin\llvm-rc.exe"
        if (Test-Path $candidate) {
            return @{ Path = $candidate; Source = "llvm-rc" }
        }
    }
    if (Test-Path "C:\Program Files\LLVM\bin\llvm-rc.exe") {
        return @{ Path = "C:\Program Files\LLVM\bin\llvm-rc.exe"; Source = "llvm-rc" }
    }
    $onPath = Get-ToolPath "llvm-rc"
    if ($onPath) {
        return @{ Path = $onPath; Source = "llvm-rc" }
    }
    return @{ Path = $null; Source = $null }
}

# Get git version
function Get-GitVersion {
    try {
        $output = (& git --version 2>&1) | Out-String
        if ($output -match 'git version (\d+\.\d+(\.\d+)?)') {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# The Python CMake configures with. cmake/Dependencies.cmake uses the project venv
# (.venv\Scripts\python.exe) whenever it exists; otherwise find_package(Python3 3.14) picks an
# installed Python >= 3.14. Tried in order: the venv, py -<min>, python, python3 -- each verified to
# run (the Windows Store "python" alias exits non-zero). Returns the venv regardless of its version
# (it is what CMake uses), else the first candidate >= the minimum, else the first working Python 3,
# so its version can be reported. Returns @{ Exe; Args; Version; Path } or $null.
function Get-PythonInfo {
    $pyVersionArg = "-$($MIN_PYTHON_VERSION.Major).$($MIN_PYTHON_VERSION.Minor)"
    $candidates = @()
    $venvPython = Join-Path $RepoRoot ".venv\Scripts\python.exe"
    if (Test-Path -LiteralPath $venvPython) {
        $candidates += , @{ Exe = $venvPython; Args = @() }
    }
    if (Get-ToolPath "py") {
        $candidates += , @{ Exe = "py"; Args = @($pyVersionArg) }
    }
    $candidates += , @{ Exe = "python"; Args = @() }
    $candidates += , @{ Exe = "python3"; Args = @() }

    $firstWorking = $null
    foreach ($candidate in $candidates) {
        try {
            $output = (& $candidate.Exe @($candidate.Args + @('-X', 'utf8', '-c', 'import sys, platform; print(platform.python_version()); print(sys.executable)')) 2>&1) | Out-String
        }
        catch { continue }
        # platform.python_version() can carry a suffix (3.15.0a1); [Version] takes only the numbers.
        if ($LASTEXITCODE -ne 0 -or $output -notmatch '(?m)^(?<ver>3\.\d+(?:\.\d+)?)\S*\s*\r?\n(?<path>.+)$') { continue }
        $info = @{ Exe = $candidate.Exe; Args = $candidate.Args; Version = $Matches['ver']; Path = $Matches['path'].Trim() }
        if ($candidate.Exe -eq $venvPython -or [Version]$info.Version -ge $MIN_PYTHON_VERSION) {
            return $info
        }
        if (-not $firstWorking) { $firstWorking = $info }
    }
    return $firstWorking
}

# jinja2's version for the given Python, or $null if it can't import it.
function Get-Jinja2Version {
    param($PythonInfo)
    if (-not $PythonInfo) { return $null }
    try {
        $result = & $PythonInfo.Exe @($PythonInfo.Args + @('-X', 'utf8', '-c', 'import jinja2; print(jinja2.__version__)')) 2>&1
        if ($LASTEXITCODE -eq 0 -and "$result" -match '^\d+\.') {
            return "$result".Trim()
        }
    }
    catch {}
    return $null
}

# Main
Write-Host ""
Write-Host "========================================" -ForegroundColor White
Write-Host "  Prerequisite Check" -ForegroundColor White
Write-Host "========================================" -ForegroundColor White
Write-Host ""
Write-Host "Tool           Version      Required     Path" -ForegroundColor White
Write-Host ('-' * 74)

$AllOK = $true

# Smart App Control first: when it enforces, the tools below can be installed and still not run (#4781).
$smartAppControl = Get-SmartAppControlState
if (-not (Write-SmartAppControlStatus -State $smartAppControl)) {
    $AllOK = $false
}
$clangVersionPattern = 'clang version (\d+)'

# Check clang and clang++ -- the compilers the win-* presets use
$clangPath = Get-PresetLlvmToolPath "clang"
if (-not (Write-LlvmToolStatus -Name "clang" -Path $clangPath -MissingHint (Get-PresetLlvmToolHint "clang") -Major (Get-ToolVersion -Path $clangPath -Pattern $clangVersionPattern) -RequirePinned)) {
    $AllOK = $false
}
$clangppPath = Get-PresetLlvmToolPath "clang++"
if (-not (Write-LlvmToolStatus -Name "clang++" -Path $clangppPath -MissingHint (Get-PresetLlvmToolHint "clang++") -Major (Get-ToolVersion -Path $clangppPath -Pattern $clangVersionPattern) -RequirePinned)) {
    $AllOK = $false
}

# The MSVC standard library clang compiles against (#4354): the floor is CI's. An older one is a warning, not a
# failure: it builds until the code uses a library feature only the newer STL has (<flat_map>, <flat_set>).
$msvcToolset = Get-MsvcToolset
if (-not $msvcToolset) {
    Write-Status -Name "MSVC STL" -Status "warn" -Version $null -Path "(Visual Studio with the MSVC C++ libraries; pwsh tools/setup-dev.ps1 installs VS 2026)" -Required $MIN_MSVC_TOOLSET_VERSION
}
elseif ($msvcToolset.Version -ge $MIN_MSVC_TOOLSET_VERSION) {
    Write-Status -Name "MSVC STL" -Status "ok" -Version $msvcToolset.Version.ToString() -Path $msvcToolset.Path -Required $MIN_MSVC_TOOLSET_VERSION
}
else {
    Write-Status -Name "MSVC STL" -Status "warn" -Version $msvcToolset.Version.ToString() -Path "$($msvcToolset.Path) (older than CI's; pwsh tools/setup-dev.ps1 installs VS 2026)" -Required $MIN_MSVC_TOOLSET_VERSION
}

# Memory per core (#4825): a warning, not a failure. Win32_ComputerSystem reads without elevation.
$computerSystem = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction SilentlyContinue
if ($computerSystem) {
    $null = Write-MemoryPerCoreStatus -MemoryBytes ([double]$computerSystem.TotalPhysicalMemory) -Cores ([int]$computerSystem.NumberOfLogicalProcessors)
}

# Developer tools, not needed to build, test or run (#2618): a missing or old one is a warning, not a
# failure. clangd and clang-format come with LLVM, whose installer doesn't add itself to PATH, so
# they're found as clang-tidy is (LLVM_ROOT first); VS Code's workspace settings resolve them from
# PATH, so a row whose tool isn't on PATH says so.
function Write-DevLlvmToolStatus {
    param([string]$Name, [string]$Pattern)
    $path = Find-LlvmTool -Name $Name
    $major = Get-ToolVersion -Path $path -Pattern $Pattern
    if ($path -and $major -and [int]$major -ge $MIN_CLANG_VERSION) {
        $null = Write-LlvmToolStatus -Name $Name -Path $path -Major $major
        if (-not (Get-ToolPath $Name)) {
            Write-Host "  (not on PATH; for VS Code, add $(Split-Path -Parent $path) to PATH)" -ForegroundColor Yellow
        }
        return
    }
    $shown = if ($path) { $path } else { "(optional dev tool; comes with LLVM $MIN_CLANG_VERSION)" }
    Write-Status -Name $Name -Status "warn" -Version $major -Path $shown -Required $MIN_CLANG_VERSION
}

Write-DevLlvmToolStatus -Name "clangd" -Pattern 'clangd version (\d+)'

# Check CMake
$cmakeVer = Get-CMakeVersion
$cmakePath = Get-ToolPath "cmake"
if ($cmakeVer -and [Version]$cmakeVer -ge $MIN_CMAKE_VERSION) {
    Write-Status -Name "cmake" -Status "ok" -Version $cmakeVer -Path $cmakePath -Required $MIN_CMAKE_VERSION
}
else {
    Write-Status -Name "cmake" -Status "fail" -Version $cmakeVer -Path $cmakePath -Required $MIN_CMAKE_VERSION
    $AllOK = $false
}

# Check Ninja
$ninjaVer = Get-NinjaVersion
$ninjaPath = Get-ToolPath "ninja"
if ($ninjaVer) {
    Write-Status -Name "ninja" -Status "ok" -Version $ninjaVer -Path $ninjaPath -Required ""
}
else {
    Write-Status -Name "ninja" -Status "fail" -Version $null -Path $ninjaPath -Required ""
    $AllOK = $false
}

# Check lld -- the lld-link beside the presets' clang++
$lldPath = Get-PresetLlvmToolPath "lld-link"
if (-not (Write-LlvmToolStatus -Name "lld" -Path $lldPath -MissingHint (Get-PresetLlvmToolHint "lld-link") -Major (Get-ToolVersion -Path $lldPath -Pattern 'LLD (\d+)'))) {
    $AllOK = $false
}

# Check ccache (recommended, not required: CONTRIBUTING.md lists it as "optional but recommended",
# and cmake/CompilerOptions.cmake only warns when neither ccache nor sccache is found)
$ccacheVer = Get-CcacheVersion
$ccachePath = Get-ToolPath "ccache"
if ($ccacheVer -and $ccacheVer -ge $MIN_CCACHE_VERSION) {
    Write-Status -Name "ccache" -Status "ok" -Version $ccacheVer -Path $ccachePath -Required $MIN_CCACHE_VERSION
}
else {
    Write-Status -Name "ccache" -Status "warn" -Version $ccacheVer -Path "(recommended for faster rebuilds)" -Required $MIN_CCACHE_VERSION
}

# Check clang-tidy -- found as tools/clang-tidy.ps1 finds it (LLVM_ROOT first)
$tidyPath = Find-LlvmTool -Name "clang-tidy"
if (-not (Write-LlvmToolStatus -Name "clang-tidy" -Path $tidyPath -Major (Get-ToolVersion -Path $tidyPath -Pattern 'LLVM version (\d+)'))) {
    $AllOK = $false
}

# Check clang-format (a developer tool: required before a pull request, not to build)
Write-DevLlvmToolStatus -Name "clang-format" -Pattern 'clang-format version (\d+)'
$formatPath = Find-LlvmTool -Name "clang-format"

# Check llvm-profdata and llvm-cov (optional, coverage/PGO) -- found as tools/coverage.ps1 finds them
$profdataPath = Find-LlvmTool -Name "llvm-profdata"
$null = Write-LlvmToolStatus -Name "llvm-profdata" -Path $profdataPath -Major (Get-ToolVersion -Path $profdataPath -Pattern 'LLVM version (\d+)' -Arguments @('show', '--version'))
$covPath = Find-LlvmTool -Name "llvm-cov"
$null = Write-LlvmToolStatus -Name "llvm-cov" -Path $covPath -Major (Get-ToolVersion -Path $covPath -Pattern 'LLVM version (\d+)')

# Optional Windows profiling tools (the Windows Performance Toolkit): a warning when missing
$wprPath = Get-ToolPath "wpr"
if ($wprPath) {
    Write-Status -Name "wpr" -Status "ok" -Version "available" -Path $wprPath -Required ""
}
else {
    Write-Status -Name "wpr" -Status "warn" -Version $null -Path "(optional: Windows Performance Toolkit, for ETW profiling)" -Required ""
}

$xperfPath = Get-ToolPath "xperf"
if ($xperfPath) {
    Write-Status -Name "xperf" -Status "ok" -Version "available" -Path $xperfPath -Required ""
}
else {
    Write-Status -Name "xperf" -Status "warn" -Version $null -Path "(optional: Windows Performance Toolkit, for ETW profiling)" -Required ""
}

$wpaPath = Get-ToolPath "wpa"
if ($wpaPath) {
    Write-Status -Name "wpa" -Status "ok" -Version "available" -Path $wpaPath -Required ""
}
else {
    Write-Status -Name "wpa" -Status "warn" -Version $null -Path "(optional: Windows Performance Toolkit, for ETW profiling)" -Required ""
}

$wpaExporterPath = Get-ToolPath "wpaexporter"
if ($wpaExporterPath) {
    Write-Status -Name "wpaexporter" -Status "ok" -Version "available" -Path $wpaExporterPath -Required ""
}
else {
    Write-Status -Name "wpaexporter" -Status "warn" -Version $null -Path "(optional: Windows Performance Toolkit, for ETW profiling)" -Required ""
}

# Check Python 3 (required for GLAD OpenGL loader generation) -- the interpreter CMake will use
$venvDir = Join-Path $RepoRoot ".venv"
$hasVenv = Test-Path -LiteralPath (Join-Path $venvDir "Scripts\python.exe")
$pythonInfo = Get-PythonInfo
$python3Ver = if ($pythonInfo) { $pythonInfo.Version } else { $null }
$python3Path = if ($pythonInfo) { $pythonInfo.Path } else { $null }
if ($python3Ver -and [Version]$python3Ver -ge $MIN_PYTHON_VERSION) {
    Write-Status -Name "python3" -Status "ok" -Version $python3Ver -Path $python3Path -Required $MIN_PYTHON_VERSION
}
else {
    Write-Status -Name "python3" -Status "fail" -Version $python3Ver -Path $python3Path -Required $MIN_PYTHON_VERSION
    $AllOK = $false
}

# The project venv (.venv), which tools/setup-dev.ps1 creates and CMake prefers. Not required:
# without it CMake falls back to an installed Python 3.14.
if ($hasVenv) {
    Write-Status -Name "venv" -Status "ok" -Version "present" -Path $venvDir -Required ""
}
else {
    Write-Status -Name "venv" -Status "warn" -Version "missing" -Path "(pwsh tools/setup-dev.ps1 creates .venv)" -Required ""
}

# Check jinja2 Python module (required for GLAD), in the Python CMake will use
$jinja2Ver = Get-Jinja2Version -PythonInfo $pythonInfo
if ($jinja2Ver) {
    Write-Status -Name "jinja2" -Status "ok" -Version $jinja2Ver -Path "(python module)" -Required ""
}
else {
    $jinja2Hint = if ($hasVenv) {
        "(.venv\Scripts\python.exe -m pip install --require-hashes -r requirements-glad.txt)"
    }
    else {
        "(pwsh tools/setup-dev.ps1, or python -m pip install --require-hashes -r requirements-glad.txt)"
    }
    Write-Status -Name "jinja2" -Status "fail" -Version "missing" -Path $jinja2Hint -Required ""
    $AllOK = $false
}

# Check git
$gitVer = Get-GitVersion
$gitPath = Get-ToolPath "git"
if ($gitVer -and [Version]$gitVer -ge $MIN_GIT_VERSION) {
    Write-Status -Name "git" -Status "ok" -Version $gitVer -Path $gitPath -Required $MIN_GIT_VERSION
}
else {
    Write-Status -Name "git" -Status "fail" -Version $gitVer -Path $gitPath -Required $MIN_GIT_VERSION
    $AllOK = $false
}

# Check for an RC compiler (required -- CMake configuration fails without one, with no fallback
# to the Windows SDK's rc.exe unless RC opts in -- see Get-RcCompilerPath above)
$rcResult = Get-RcCompilerPath
if ($rcResult.Path) {
    Write-Status -Name $rcResult.Source -Status "ok" -Version "available" -Path $rcResult.Path -Required ""
}
else {
    Write-Status -Name "llvm-rc" -Status "fail" -Version $null -Path $null -Required ""
    $AllOK = $false
}

Write-Host ('-' * 74)
if ($smartAppControl -eq 1) {
    foreach ($line in Get-SmartAppControlMessage) {
        Write-Host "$([char]0x2717) " -ForegroundColor Red -NoNewline
        Write-Host $line -ForegroundColor Red
    }
}
elseif ($smartAppControl -eq 2) {
    Write-Host "! " -ForegroundColor Yellow -NoNewline
    Write-Host "Smart App Control is in evaluation mode and may switch itself to enforce, which blocks unsigned developer tools and every TaskSmack binary you build. Turn it off in Windows Security > App & browser control > Smart App Control settings if you develop here (it can't be turned back on without reinstalling Windows). About it: $SmartAppControlHelpUrl" -ForegroundColor Yellow
}
if ($script:LlvmCompilerMajor) {
    Write-Host "$([char]0x2717) " -ForegroundColor Red -NoNewline
    Write-Host "The win-* presets build with LLVM $PinnedLlvmMajor (.pre-commit-config.yaml), but this clang is LLVM $($script:LlvmCompilerMajor). pwsh tools/setup-dev.ps1 replaces it with LLVM $PinnedLlvmMajor." -ForegroundColor Red
}
if ($script:LlvmSkewSeen) {
    Write-Host "! " -ForegroundColor Yellow -NoNewline
    Write-Host "LLVM $PinnedLlvmMajor is the version CI pins (.pre-commit-config.yaml); rows marked ! are another major." -ForegroundColor Yellow
    if ($clangppPath -and -not $script:LlvmCompilerMajor) {
        Write-LlvmVersionSkewWarning -ToolPath $clangppPath -Name 'clang++'
    }
    if ($formatPath) {
        Write-LlvmVersionSkewWarning -ToolPath $formatPath -Name 'clang-format'
    }
}
Write-Host ""

# Environment variables
Write-Host "Environment Variables" -ForegroundColor White
Write-Host ('-' * 74)

if ($env:CMAKE_ROOT) {
    Write-Host "$([char]0x2713) " -ForegroundColor Green -NoNewline
    Write-Host "CMAKE_ROOT      = " -NoNewline
    Write-Host $env:CMAKE_ROOT -ForegroundColor Blue
}
else {
    Write-Host "$([char]0x25CB) " -ForegroundColor Yellow -NoNewline
    Write-Host "CMAKE_ROOT      = " -NoNewline
    Write-Host "(not set)" -ForegroundColor Yellow
}

# Required: the win-* presets derive the compiler paths from it (CMakePresets.json, windows-base).
if ($env:LLVM_ROOT) {
    Write-Host "$([char]0x2713) " -ForegroundColor Green -NoNewline
    Write-Host "LLVM_ROOT       = " -NoNewline
    Write-Host $env:LLVM_ROOT -ForegroundColor Blue
}
else {
    Write-Host "$([char]0x2717) " -ForegroundColor Red -NoNewline
    Write-Host "LLVM_ROOT       = " -NoNewline
    Write-Host "(not set; the win-* presets need it -- set it to your LLVM $(if ($PinnedLlvmMajor) { $PinnedLlvmMajor } else { $MIN_CLANG_VERSION }) install, e.g. C:\Program Files\LLVM)" -ForegroundColor Red
    $AllOK = $false
}

if ($env:CC) {
    Write-Host "$([char]0x2713) " -ForegroundColor Green -NoNewline
    Write-Host "CC              = " -NoNewline
    Write-Host $env:CC -ForegroundColor Blue
}
else {
    Write-Host "$([char]0x25CB) " -ForegroundColor Yellow -NoNewline
    Write-Host "CC              = " -NoNewline
    Write-Host "(not set, using default)" -ForegroundColor Yellow
}

if ($env:CXX) {
    Write-Host "$([char]0x2713) " -ForegroundColor Green -NoNewline
    Write-Host "CXX             = " -NoNewline
    Write-Host $env:CXX -ForegroundColor Blue
}
else {
    Write-Host "$([char]0x25CB) " -ForegroundColor Yellow -NoNewline
    Write-Host "CXX             = " -NoNewline
    Write-Host "(not set, using default)" -ForegroundColor Yellow
}

Write-Host ('-' * 74)
Write-Host ""

# Summary
if ($AllOK) {
    Write-Host "All required prerequisites are installed and meet version requirements." -ForegroundColor Green
    exit 0
}
else {
    Write-Host "Some prerequisites are missing or do not meet version requirements." -ForegroundColor Red
    Write-Host "Run " -NoNewline
    Write-Host "pwsh tools/setup-dev.ps1 -DryRun" -ForegroundColor Cyan -NoNewline
    Write-Host " to see what tools/setup-dev.ps1 would install."
    exit 1
}
