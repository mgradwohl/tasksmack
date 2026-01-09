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
.EXAMPLE
    .\clang-tidy.ps1
.EXAMPLE
    .\clang-tidy.ps1 -ShowDetails -Jobs 8
.EXAMPLE
    .\clang-tidy.ps1 -Files src/Domain/ProcessModel.cpp
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("debug", "relwithdebinfo")]
    [string]$BuildType = "debug",

    [switch]$ShowDetails,

    [int]$Jobs = 0,

    [string[]]$Files = @()
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

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

# Find clang-tidy
$ClangTidy = $null
$SearchPaths = @(
    "$env:LLVM_ROOT\bin\clang-tidy.exe",
    "$env:ProgramFiles\LLVM\bin\clang-tidy.exe",
    "C:\Program Files\LLVM\bin\clang-tidy.exe"
)
foreach ($path in $SearchPaths) {
    if (Test-Path $path) {
        $ClangTidy = $path
        break
    }
}
# Try PATH as fallback
if (-not $ClangTidy) {
    $ClangTidy = Get-Command clang-tidy -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}
if (-not $ClangTidy) {
    Write-Error "clang-tidy not found. Please install LLVM or set LLVM_ROOT."
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

# Ensure compile_commands.json exists
if (-not (Test-Path $CompileCommandsJson)) {
    Write-Host "Building to generate compile_commands.json..."
    & cmake --build $BuildDir --target copy-compile-commands
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Strip C++20 module flags from compile_commands.json (clang-tidy doesn't handle them)
# Note: PCH flags are no longer included in compile_commands.json by CMake/Ninja
if ($ShowDetails) {
    Write-Host "Stripping module flags from compile_commands.json..."
}
$content = Get-Content $CompileCommandsJson -Raw
$content = $content -replace '@[^ ]*\.modmap', ''
$content = $content -replace '-fmodule-output=[^ ]*', ''
Set-Content $CompileCommandsJson -Value $content -NoNewline

# Determine files to analyze
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

if ($SourceFiles.Count -eq 0) {
    Write-Host "No source files found to analyze."
    exit 0
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
    $buildDir = $using:BuildDir
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
