<#
.SYNOPSIS
    Run clang-tidy on source files (Windows).
.DESCRIPTION
    Configures if needed, then runs clang-tidy with parallel execution.
    Uses .clang-tidy configuration from project root.
.PARAMETER BuildType
    Build type to use: debug, relwithdebinfo (default: debug)
.PARAMETER ShowDetails
    Show verbose output with per-file progress
.PARAMETER Jobs
    Number of parallel jobs (default: number of CPU cores)
.PARAMETER Files
    Specific files to analyze (default: all source files)
.PARAMETER ChangedOnly
    Only analyze files that have been modified according to git
.EXAMPLE
    .\clang-tidy.ps1
.EXAMPLE
    .\clang-tidy.ps1 -ShowDetails -Jobs 8
.EXAMPLE
    .\clang-tidy.ps1 -Files src/Domain/ProcessModel.cpp
.EXAMPLE
    .\clang-tidy.ps1 -ChangedOnly
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("debug", "relwithdebinfo")]
    [string]$BuildType = "debug",

    [switch]$ShowDetails,

    [int]$Jobs = 0,

    [string[]]$Files = @(),

    [switch]$ChangedOnly
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
. (Join-Path $ScriptDir "common.ps1")
# CMake presets derive compiler paths from LLVM_ROOT; ensure it is set
# before any cmake --preset recovery path below runs.
Initialize-LlvmRoot

# Limit header diagnostics to project headers.
# Note: clang-tidy requires --header-filter to be set when using --exclude-header-filter.
$ProjectRootRegex = [regex]::Escape($ProjectRoot)
$HeaderFilterRegex = "^$ProjectRootRegex[\\/](src|tests)[\\/]"

# Exclude generated/build trees and the other platform's folder.
# gladsources is generated under build\<preset>\gladsources.
$ExcludeHeaderFilterRegex = "^$ProjectRootRegex[\\/](build|dist|coverage|\.cache)[\\/]|^$ProjectRootRegex[\\/]src[\\/]Platform[\\/]Linux[\\/]|^$ProjectRootRegex.*[\\/]gladsources[\\/]"

# Build directory
$BuildDir = Join-Path $ProjectRoot "build\win-$BuildType"
$CompileCommandsJson = Join-Path $BuildDir "compile_commands.json"
$TidyCompdbDir = Join-Path $BuildDir "clang-tidy-compdb"
$CompileCommandsTidy = Join-Path $TidyCompdbDir "compile_commands.json"

# Find clang-tidy
$ClangTidy = Find-LlvmTool -Name "clang-tidy"
if (-not $ClangTidy) {
    Write-Error "clang-tidy not found. Please install LLVM or set LLVM_ROOT." -ErrorAction Continue
    exit 1
}

if ($ShowDetails) {
    Write-Host "Using clang-tidy: $ClangTidy"
}

# Configure if needed (using CMake presets)
$NinjaFile = Join-Path $BuildDir "build.ninja"
if (-not (Test-Path $NinjaFile)) {
    Write-Host "Build not configured. Running cmake --preset win-$BuildType..."
    & cmake --preset "win-$BuildType"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Ensure compile_commands.json exists (emitted by CMake at generate time)
if (-not (Test-Path $CompileCommandsJson)) {
    Write-Host "compile_commands.json missing. Re-running cmake --preset win-$BuildType..."
    & cmake --preset "win-$BuildType"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Generate a sanitized compile_commands.json for clang-tidy without touching the live file.
# CMake emits two PCH-related flag groups per TU:
#   1. -Xclang -include-pch -Xclang /path/cmake_pch.hxx.pch  (binary PCH)
#   2. -Xclang -include    -Xclang /path/cmake_pch.hxx        (PCH source include)
#   3. -Xclang -fno-pch-timestamp                            (PCH reproducibility flag, defensive)
# clang-tidy cannot use any of these without a prior full build, so strip all.
if ($ShowDetails) {
    Write-Host "Generating sanitized clang-tidy compilation database..."
}
Remove-Item $CompileCommandsTidy -Force -ErrorAction SilentlyContinue
& cmake --build $BuildDir --target generate-clang-tidy-compile-commands 2>$null
if ($LASTEXITCODE -ne 0 -and $ShowDetails) {
    Write-Host "Falling back to local compile_commands sanitization..."
}
if (-not (Test-Path $CompileCommandsTidy)) {
    $null = New-Item -ItemType Directory -Path $TidyCompdbDir -Force
    $content = Get-Content $CompileCommandsJson -Raw
    $content = $content -replace '@[^ ]*\.modmap', ''
    $content = $content -replace '-fmodule-output=[^ ]*', ''
    $content = $content -replace '-Xclang -include-pch -Xclang [^ ]*', ''
    $content = $content -replace '-Xclang -include -Xclang [^ ]*cmake_pch[^ ]*', ''
    $content = $content -replace '-Xclang -fno-pch-timestamp', ''
    Set-Content $CompileCommandsTidy -Value $content -NoNewline
}

# Determine files to analyze
if ($ChangedOnly) {
    # Get changed files from git
    if ($ShowDetails) {
        Write-Host "Getting changed files from git..."
    }
    $gitOutput = & git diff --name-only HEAD 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Failed to get changed files from git. Falling back to all files."
        $ChangedOnly = $false
    } else {
        $SourceFiles = $gitOutput |
            Where-Object { $_ -match '\.(cpp|h)$' } |
            Where-Object { $_ -notmatch 'Platform[/\\]Linux[/\\]' } |
            ForEach-Object { Join-Path $ProjectRoot $_ }
        if ($SourceFiles.Count -eq 0) {
            Write-Host "No changed C++ files found."
            exit 0
        }
    }
}

if (-not $ChangedOnly) {
    if ($Files.Count -eq 0) {
        # Get all source files from project, excluding other-platform files
        $SourceFiles = @()
        $SourceDirs = @("src")
        foreach ($dir in $SourceDirs) {
            $fullDir = Join-Path $ProjectRoot $dir
            if (Test-Path $fullDir) {
                $SourceFiles += Get-ChildItem -Path $fullDir -Recurse -Include "*.cpp" |
                    Where-Object { $_.FullName -notmatch '\\Platform\\Linux\\' } |
                    Select-Object -ExpandProperty FullName
            }
        }
    } else {
        $SourceFiles = $Files | ForEach-Object {
            if ([System.IO.Path]::IsPathRooted($_)) { $_ }
            else { Join-Path $ProjectRoot $_ }
        }
    }
}

if ($SourceFiles.Count -eq 0) {
    Write-Host "Error: No source files found to analyze." -ForegroundColor Red
    exit 1
}

# Determine number of jobs
if ($Jobs -le 0) {
    $Jobs = [Environment]::ProcessorCount
}

if ($ShowDetails) {
    Write-Host "Running clang-tidy on $($SourceFiles.Count) files with $Jobs parallel jobs..."
    Write-Host ""
}

# Run clang-tidy in parallel using PowerShell jobs
$ConfigFile = Join-Path $ProjectRoot ".clang-tidy"

# Process files in batches for parallel execution
$results = $SourceFiles | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $file = $_
    $clangTidy = $using:ClangTidy
    $configFile = $using:ConfigFile
    $buildDir = $using:TidyCompdbDir
    $projectRoot = $using:ProjectRoot
    $showDetails = $using:ShowDetails

    # Get relative path for display
    $relativePath = $file
    if ($file.StartsWith($projectRoot)) {
        $relativePath = $file.Substring($projectRoot.Length + 1)
    }

    if ($showDetails) {
        Write-Host "  Analyzing: $relativePath"
    }

    $output = & $clangTidy `
        --config-file="$configFile" `
        --header-filter="$using:HeaderFilterRegex" `
        --exclude-header-filter="$using:ExcludeHeaderFilterRegex" `
        -p "$buildDir" `
        --extra-arg=-std=c++23 `
        --extra-arg=-Wno-unknown-warning-option `
        "$file" 2>&1

    $exitCode = $LASTEXITCODE

    [PSCustomObject]@{
        File = $relativePath
        ExitCode = $exitCode
        Output = $output
    }
}

# Collect results
$hasErrors = $false
foreach ($result in $results) {
    if ($result.ExitCode -ne 0 -or ($result.Output -match "warning:|error:")) {
        if ($result.Output) {
            Write-Host $result.Output
        }
        if ($result.ExitCode -ne 0) {
            $hasErrors = $true
        }
    }
}

Write-Host ""
if ($hasErrors) {
    Write-Host "clang-tidy found issues." -ForegroundColor Yellow
    exit 1
} else {
    Write-Host "clang-tidy completed successfully." -ForegroundColor Green
    exit 0
}
