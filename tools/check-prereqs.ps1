<#
.SYNOPSIS
    Check Prerequisites Script for Windows.
.DESCRIPTION
    Displays all required tools, their paths, versions, and status.
.EXAMPLE
    .\check-prereqs.ps1
#>
[CmdletBinding()]
param()

# Required versions (minimum)
$MIN_CMAKE_VERSION = [Version]"3.28"
$MIN_CLANG_VERSION = 22
$MIN_CCACHE_VERSION = [Version]"4.9.1"

# Helper function to print status
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
    $requiredFormatted = if ($Required) { "(>= $Required)".PadRight(12) } else { "".PadRight(12) }
    
    if ($Status -eq "ok") {
        Write-Host "✓ " -ForegroundColor Green -NoNewline
        Write-Host $nameFormatted -NoNewline
        Write-Host $versionFormatted -ForegroundColor Green -NoNewline
    }
    else {
        Write-Host "✗ " -ForegroundColor Red -NoNewline
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

# Get clang version
function Get-ClangVersion {
    try {
        $output = (& clang --version 2>&1) | Out-String
        if ($output -match 'clang version (\d+)') {
            return [int]$Matches[1]
        }
    }
    catch {}
    return $null
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

# Get ccache version
function Get-CcacheVersion {
    try {
        $output = (& ccache --version 2>&1) | Out-String
        if ($output -match 'ccache version (\d+\.\d+\.\d+)') {
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

# Get lld version
function Get-LLDVersion {
    try {
        $output = (& lld-link --version 2>&1) | Out-String
        if ($output -match 'LLD (\d+\.\d+)') {
            return $Matches[1]
        }
    }
    catch {}
    try {
        $output = (& ld.lld --version 2>&1) | Out-String
        if ($output -match 'LLD (\d+\.\d+)') {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# Get clang-tidy version
function Get-ClangTidyVersion {
    try {
        $output = (& clang-tidy --version 2>&1) | Out-String
        if ($output -match 'LLVM version (\d+)') {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# Get clang-format version
function Get-ClangFormatVersion {
    try {
        $output = (& clang-format --version 2>&1) | Out-String
        if ($output -match 'clang-format version (\d+)') {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# Get llvm-profdata version
function Get-LLVMProfdataVersion {
    try {
        $output = (& llvm-profdata show --version 2>&1) | Out-String
        if ($output -match 'LLVM version (\d+)') {
            return $Matches[1]
        }
    }
    catch {}
    return $null
}

# Get llvm-cov version
function Get-LLVMCovVersion {
    try {
        $output = (& llvm-cov --version 2>&1) | Out-String
        if ($output -match 'LLVM version (\d+)') {
            return $Matches[1]
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
Write-Host "──────────────────────────────────────────────────────────────────────────"

$AllOK = $true

# Check Clang
$clangVer = Get-ClangVersion
$clangPath = Get-ToolPath "clang"
if ($clangVer -and $clangVer -ge $MIN_CLANG_VERSION) {
    Write-Status -Name "clang" -Status "ok" -Version $clangVer -Path $clangPath -Required $MIN_CLANG_VERSION
}
else {
    Write-Status -Name "clang" -Status "fail" -Version $clangVer -Path $clangPath -Required $MIN_CLANG_VERSION
    $AllOK = $false
}

# Check Clang++
$clangppPath = Get-ToolPath "clang++"
if ($clangVer -and $clangVer -ge $MIN_CLANG_VERSION) {
    Write-Status -Name "clang++" -Status "ok" -Version $clangVer -Path $clangppPath -Required $MIN_CLANG_VERSION
}
else {
    Write-Status -Name "clang++" -Status "fail" -Version $clangVer -Path $clangppPath -Required $MIN_CLANG_VERSION
    $AllOK = $false
}

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

# Check lld
$lldVer = Get-LLDVersion
$lldPath = Get-ToolPath "lld-link"
if (-not $lldPath) { $lldPath = Get-ToolPath "ld.lld" }
if ($lldVer) {
    Write-Status -Name "lld" -Status "ok" -Version $lldVer -Path $lldPath -Required ""
}
else {
    Write-Status -Name "lld" -Status "fail" -Version $null -Path $lldPath -Required ""
    $AllOK = $false
}

# Check ccache
$ccacheVer = Get-CcacheVersion
$ccachePath = Get-ToolPath "ccache"
if ($ccacheVer -and $ccacheVer -ge $MIN_CCACHE_VERSION) {
    Write-Status -Name "ccache" -Status "ok" -Version $ccacheVer -Path $ccachePath -Required $MIN_CCACHE_VERSION
}
else {
    Write-Status -Name "ccache" -Status "fail" -Version $ccacheVer -Path $ccachePath -Required $MIN_CCACHE_VERSION
    $AllOK = $false
}

# Check clang-tidy
$tidyVer = Get-ClangTidyVersion
$tidyPath = Get-ToolPath "clang-tidy"
if ($tidyVer) {
    Write-Status -Name "clang-tidy" -Status "ok" -Version $tidyVer -Path $tidyPath -Required ""
}
else {
    Write-Status -Name "clang-tidy" -Status "fail" -Version $null -Path $tidyPath -Required ""
    $AllOK = $false
}

# Check clang-format
$formatVer = Get-ClangFormatVersion
$formatPath = Get-ToolPath "clang-format"
if ($formatVer) {
    Write-Status -Name "clang-format" -Status "ok" -Version $formatVer -Path $formatPath -Required ""
}
else {
    Write-Status -Name "clang-format" -Status "fail" -Version $null -Path $formatPath -Required ""
    $AllOK = $false
}

# Check llvm-profdata (optional)
$profdataVer = Get-LLVMProfdataVersion
$profdataPath = Get-ToolPath "llvm-profdata"
if ($profdataVer) {
    Write-Status -Name "llvm-profdata" -Status "ok" -Version $profdataVer -Path $profdataPath -Required ""
}
else {
    Write-Status -Name "llvm-profdata" -Status "fail" -Version $null -Path $profdataPath -Required ""
    # Don't fail for optional coverage tools
}

# Check llvm-cov (optional)
$covVer = Get-LLVMCovVersion
$covPath = Get-ToolPath "llvm-cov"
if ($covVer) {
    Write-Status -Name "llvm-cov" -Status "ok" -Version $covVer -Path $covPath -Required ""
}
else {
    Write-Status -Name "llvm-cov" -Status "fail" -Version $null -Path $covPath -Required ""
    # Don't fail for optional coverage tools
}

Write-Host "──────────────────────────────────────────────────────────────────────────"
Write-Host ""

# Environment variables
Write-Host "Environment Variables" -ForegroundColor White
Write-Host "──────────────────────────────────────────────────────────────────────────"

if ($env:CMAKE_ROOT) {
    Write-Host "✓ " -ForegroundColor Green -NoNewline
    Write-Host "CMAKE_ROOT      = " -NoNewline
    Write-Host $env:CMAKE_ROOT -ForegroundColor Blue
}
else {
    Write-Host "○ " -ForegroundColor Yellow -NoNewline
    Write-Host "CMAKE_ROOT      = " -NoNewline
    Write-Host "(not set)" -ForegroundColor Yellow
}

if ($env:LLVM_ROOT) {
    Write-Host "✓ " -ForegroundColor Green -NoNewline
    Write-Host "LLVM_ROOT       = " -NoNewline
    Write-Host $env:LLVM_ROOT -ForegroundColor Blue
}
else {
    Write-Host "○ " -ForegroundColor Yellow -NoNewline
    Write-Host "LLVM_ROOT       = " -NoNewline
    Write-Host "(not set)" -ForegroundColor Yellow
}

if ($env:CC) {
    Write-Host "✓ " -ForegroundColor Green -NoNewline
    Write-Host "CC              = " -NoNewline
    Write-Host $env:CC -ForegroundColor Blue
}
else {
    Write-Host "○ " -ForegroundColor Yellow -NoNewline
    Write-Host "CC              = " -NoNewline
    Write-Host "(not set, using default)" -ForegroundColor Yellow
}

if ($env:CXX) {
    Write-Host "✓ " -ForegroundColor Green -NoNewline
    Write-Host "CXX             = " -NoNewline
    Write-Host $env:CXX -ForegroundColor Blue
}
else {
    Write-Host "○ " -ForegroundColor Yellow -NoNewline
    Write-Host "CXX             = " -NoNewline
    Write-Host "(not set, using default)" -ForegroundColor Yellow
}

Write-Host "──────────────────────────────────────────────────────────────────────────"
Write-Host ""

# Summary
if ($AllOK) {
    Write-Host "All required prerequisites are installed and meet version requirements." -ForegroundColor Green
    exit 0
}
else {
    Write-Host "Some prerequisites are missing or do not meet version requirements." -ForegroundColor Red
    Write-Host "Run " -NoNewline
    Write-Host ".\setup.ps1 -Help" -ForegroundColor Cyan -NoNewline
    Write-Host " for installation instructions."
    exit 1
}
