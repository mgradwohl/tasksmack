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
. (Join-Path $ScriptDir "common.ps1")

Write-Host "Checking clang-format compliance..."

# Find clang-format
$ClangFormat = Find-LlvmTool -Name "clang-format"
if (-not $ClangFormat) {
    Write-Error "clang-format not found. Set LLVM_ROOT or add clang-format to PATH."
    exit 1
}
Write-LlvmVersionSkewWarning -ToolPath $ClangFormat -Name 'clang-format'

# Find all source files. None found is never a real result (a filter that matched the checkout's own
# path once passed with "All 0 files", #3528), so it fails.
$files = @(Get-FormatSourceFile -Root $ProjectRoot)
$fileCount = $files.Count
if ($fileCount -eq 0) {
    Write-Host "No C++ sources found under $ProjectRoot (src, tests, benchmarks)." -ForegroundColor Red
    exit 1
}

$failed = 0

foreach ($file in $files) {
    if ($ShowDetails) {
        Write-Host "Checking: $($file.Name)"
    }
    $null = & $ClangFormat --dry-run --Werror $file.FullName 2>&1
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
