<#
.SYNOPSIS
    Apply clang-format to source files (Windows).
.DESCRIPTION
    Formats all source files in-place. Use check-format.ps1 to check without modifying.
    Uses .clang-format configuration from project root.
.PARAMETER ShowDetails
    Show per-file progress
.PARAMETER ChangedOnly
    Only format files that have been modified according to git
.EXAMPLE
    .\clang-format.ps1
.EXAMPLE
    .\clang-format.ps1 -ShowDetails
.EXAMPLE
    .\clang-format.ps1 -ChangedOnly
#>
[CmdletBinding()]
param(
    [switch]$ShowDetails,

    [switch]$ChangedOnly
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "Applying clang-format to source files..."

# Find clang-format
$ClangFormat = $null
if ($env:LLVM_ROOT) {
    $ClangFormat = Join-Path $env:LLVM_ROOT "bin\clang-format.exe"
    if (-not (Test-Path $ClangFormat)) { $ClangFormat = $null }
}
if (-not $ClangFormat) {
    $ClangFormat = Get-Command clang-format -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}
if (-not $ClangFormat) {
    Write-Error "clang-format not found. Set LLVM_ROOT or add clang-format to PATH."
    exit 1
}

# Find source files
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
        $files = $gitOutput |
            Where-Object { $_ -match '\.(cpp|h)$' } |
            ForEach-Object { Get-Item (Join-Path $ProjectRoot $_) -ErrorAction SilentlyContinue } |
            Where-Object { $null -ne $_ }
        if ($files.Count -eq 0) {
            Write-Host "No changed C++ files found."
            exit 0
        }
    }
}

if (-not $ChangedOnly) {
    # Find all source files
    $files = Get-ChildItem -Path (Join-Path $ProjectRoot "src"), (Join-Path $ProjectRoot "tests") -Recurse -Include "*.cpp", "*.h" -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -notmatch "\\build\\" -and $_.FullName -notmatch "\\.git\\" }
}

$fileCount = ($files | Measure-Object).Count
$checkedCount = 0

foreach ($file in $files) {
    $checkedCount++
    if ($ShowDetails) {
        Write-Host "[$checkedCount/$fileCount] Formatting: $($file.Name)"
    }
    & $ClangFormat -i $file.FullName
    if ($LASTEXITCODE -ne 0) {
        Write-Error "clang-format failed on $($file.Name)"
        exit $LASTEXITCODE
    }
}

Write-Host "Formatted $fileCount files." -ForegroundColor Green
exit 0
