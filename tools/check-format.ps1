<#
.SYNOPSIS
    Check clang-format compliance without modifying files (Windows).
.DESCRIPTION
    Exit code 0 = all files formatted correctly.
    Exit code 1 = formatting issues found.
.PARAMETER ShowDetails
    Show per-file progress
.EXAMPLE
    .\check-format.ps1
.EXAMPLE
    .\check-format.ps1 -ShowDetails
#>
[CmdletBinding()]
param(
    [switch]$ShowDetails
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "Checking clang-format compliance..."

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

# Find all source files
$files = Get-ChildItem -Path (Join-Path $ProjectRoot "src"), (Join-Path $ProjectRoot "tests") -Recurse -Include "*.cpp", "*.h" -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -notmatch "\\build\\" -and $_.FullName -notmatch "\\.git\\" }

$failed = 0
$fileCount = ($files | Measure-Object).Count

foreach ($file in $files) {
    if ($ShowDetails) {
        Write-Host "Checking: $($file.Name)"
    }
    $result = & $ClangFormat --dry-run --Werror $file.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $($file.FullName)" -ForegroundColor Red
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Host "$failed of $fileCount files need formatting." -ForegroundColor Red
    Write-Host "Run 'tools\clang-format.ps1' to fix."
    exit 1
} else {
    Write-Host "All $fileCount files are correctly formatted." -ForegroundColor Green
    exit 0
}
