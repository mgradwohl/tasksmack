<#
.SYNOPSIS
    Project Setup Script for Windows.
.DESCRIPTION
    Checks prerequisites, installs them if needed, then renames placeholder
    "MyProject" to your project name throughout the codebase.
.PARAMETER Name
    Your project name (e.g., "AwesomeApp")
.PARAMETER Author
    Author name for documentation (optional)
.PARAMETER SkipPrereqs
    Skip prerequisite checking
.PARAMETER ShowDetails
    Show detailed progress
.EXAMPLE
    .\setup.ps1 -Name "MyCoolProject"
.EXAMPLE
    .\setup.ps1 -Name "DataProcessor" -Author "Jane Doe" -ShowDetails
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Author = "",

    [switch]$SkipPrereqs,

    [switch]$ShowDetails
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Required versions (minimum)
$MIN_CMAKE_VERSION = [Version]"3.28"
$MIN_CLANG_VERSION = 22
$MIN_CCACHE_VERSION = [Version]"4.9.1"

# Helper functions for colored output
function Write-Info { Write-Host "[INFO] " -ForegroundColor Blue -NoNewline; Write-Host $args }
function Write-Success { Write-Host "[OK] " -ForegroundColor Green -NoNewline; Write-Host $args }
function Write-Warn { Write-Host "[WARN] " -ForegroundColor Yellow -NoNewline; Write-Host $args }
function Write-Err { Write-Host "[ERROR] " -ForegroundColor Red -NoNewline; Write-Host $args }

# Get tool versions
function Get-ClangVersion {
    try {
        $output = & clang --version 2>$null
        if ($output -match 'clang version (\d+)') {
            return [int]$Matches[1]
        }
    } catch {}
    return $null
}

function Get-CMakeVersion {
    try {
        $output = & cmake --version 2>$null
        if ($output -match 'cmake version (\d+\.\d+)') {
            return [Version]$Matches[1]
        }
    } catch {}
    return $null
}

function Get-CcacheVersion {
    try {
        $output = & ccache --version 2>$null
        if ($output -match 'ccache version (\d+\.\d+\.\d+)') {
            return [Version]$Matches[1]
        }
    } catch {}
    return $null
}

function Get-NinjaVersion {
    try {
        $output = & ninja --version 2>$null
        return $output.Trim()
    } catch {}
    return $null
}

function Test-LLD {
    try {
        $output = & lld-link --version 2>$null
        if ($output) { return $true }
    } catch {}
    # Also check for ld.lld
    try {
        $output = & ld.lld --version 2>$null
        if ($output) { return $true }
    } catch {}
    return $false
}

function Test-LLVMTools {
    try {
        $profdata = Get-Command llvm-profdata -ErrorAction SilentlyContinue
        $cov = Get-Command llvm-cov -ErrorAction SilentlyContinue
        return ($profdata -and $cov)
    } catch {}
    return $false
}

function Get-ClangTidyVersion {
    try {
        $output = & clang-tidy --version 2>$null
        if ($output -match 'LLVM version (\d+)') {
            return [int]$Matches[1]
        }
    } catch {}
    return $null
}

function Get-ClangFormatVersion {
    try {
        $output = & clang-format --version 2>$null
        if ($output -match 'clang-format version (\d+)') {
            return [int]$Matches[1]
        }
    } catch {}
    return $null
}

function Test-Chocolatey {
    try {
        $choco = Get-Command choco -ErrorAction SilentlyContinue
        return $choco -ne $null
    } catch {}
    return $false
}

function Test-Scoop {
    try {
        $scoop = Get-Command scoop -ErrorAction SilentlyContinue
        return $scoop -ne $null
    } catch {}
    return $false
}

function Test-Winget {
    try {
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        return $winget -ne $null
    } catch {}
    return $false
}

function Install-Prerequisites {
    Write-Info "Attempting to install missing prerequisites..."
    
    $installed = $false
    
    if (Test-Chocolatey) {
        Write-Info "Using Chocolatey to install prerequisites..."
        try {
            # Check what's missing and install
            if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
                Write-Info "Installing LLVM..."
                choco install llvm -y
            }
            if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
                Write-Info "Installing CMake..."
                choco install cmake --installargs '"ADD_CMAKE_TO_PATH=System"' -y
            }
            if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
                Write-Info "Installing Ninja..."
                choco install ninja -y
            }
            if (-not (Get-Command ccache -ErrorAction SilentlyContinue)) {
                Write-Info "Installing ccache..."
                choco install ccache -y
            }
            $installed = $true
        } catch {
            Write-Warn "Chocolatey installation encountered an error: $_"
        }
    }
    elseif (Test-Scoop) {
        Write-Info "Using Scoop to install prerequisites..."
        try {
            # Add extras bucket for some tools
            scoop bucket add extras 2>$null
            scoop bucket add main 2>$null
            
            if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
                Write-Info "Installing LLVM..."
                scoop install llvm
            }
            if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
                Write-Info "Installing CMake..."
                scoop install cmake
            }
            if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
                Write-Info "Installing Ninja..."
                scoop install ninja
            }
            if (-not (Get-Command ccache -ErrorAction SilentlyContinue)) {
                Write-Info "Installing ccache..."
                scoop install ccache
            }
            $installed = $true
        } catch {
            Write-Warn "Scoop installation encountered an error: $_"
        }
    }
    elseif (Test-Winget) {
        Write-Info "Using winget to install prerequisites..."
        try {
            if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
                Write-Info "Installing LLVM..."
                winget install LLVM.LLVM --silent --accept-package-agreements --accept-source-agreements
            }
            if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
                Write-Info "Installing CMake..."
                winget install Kitware.CMake --silent --accept-package-agreements --accept-source-agreements
            }
            if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
                Write-Info "Installing Ninja..."
                winget install Ninja-build.Ninja --silent --accept-package-agreements --accept-source-agreements
            }
            $installed = $true
        } catch {
            Write-Warn "Winget installation encountered an error: $_"
        }
    }
    else {
        Write-Err "No package manager found (Chocolatey, Scoop, or winget)."
        Write-Err "Please install prerequisites manually:"
        Write-Err "  - LLVM/Clang $MIN_CLANG_VERSION+ (includes clang-tidy, clang-format, lld)"
        Write-Err "  - CMake $MIN_CMAKE_VERSION+"
        Write-Err "  - Ninja"
        Write-Err "  - ccache $MIN_CCACHE_VERSION+"
        Write-Host ""
        Write-Info "Recommended: Install Chocolatey from https://chocolatey.org/install"
        Write-Info "Then run: choco install llvm cmake ninja ccache -y"
        Write-Info "(LLVM package includes clang-tidy, clang-format, lld, and llvm-cov)"
        return $false
    }
    
    # Refresh PATH
    if ($installed) {
        Write-Info "Refreshing environment PATH..."
        $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
    }
    
    return $installed
}

function Show-PathFix {
    param([string]$Tool, [string]$CurrentVersion, [string]$RequiredVersion)
    
    Write-Host ""
    Write-Warn "The '$Tool' in your PATH reports version $CurrentVersion, but $RequiredVersion+ is required."
    Write-Host ""
    
    switch ($Tool) {
        "clang" {
            Write-Info "Try one of these fixes:"
            Write-Host "  1. Set LLVM_ROOT environment variable:"
            Write-Host '     $env:LLVM_ROOT = "C:\Program Files\LLVM"'
            Write-Host ""
            Write-Host "  2. Add LLVM bin to your PATH:"
            Write-Host '     $env:Path = "C:\Program Files\LLVM\bin;" + $env:Path'
            Write-Host ""
            Write-Host "  3. Use Visual Studio Developer PowerShell with clang:"
            Write-Host "     - Open 'Developer PowerShell for VS 2022'"
            Write-Host "     - Or run: Import-Module 'C:\Program Files\Microsoft Visual Studio\2022\...\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'"
        }
        "cmake" {
            Write-Info "Try one of these fixes:"
            Write-Host "  1. Download latest CMake from cmake.org"
            Write-Host ""
            Write-Host "  2. Update via package manager:"
            Write-Host "     choco upgrade cmake -y"
            Write-Host "     # or"
            Write-Host "     scoop update cmake"
        }
        "ccache" {
            Write-Info "Try one of these fixes:"
            Write-Host "  1. Update via package manager:"
            Write-Host "     choco upgrade ccache -y"
            Write-Host "     # or"
            Write-Host "     scoop update ccache"
            Write-Host ""
            Write-Host "  2. Download from: https://ccache.dev/download.html"
        }
    }
}

function Test-Prerequisites {
    Write-Host ""
    Write-Info "========================================"
    Write-Info "  Checking Prerequisites"
    Write-Info "========================================"
    Write-Host ""
    
    $needsInstall = $false
    
    # Check Clang
    $clangVer = Get-ClangVersion
    if ($null -eq $clangVer) {
        Write-Warn "Clang: NOT INSTALLED"
        $needsInstall = $true
    }
    elseif ($clangVer -lt $MIN_CLANG_VERSION) {
        Write-Warn "Clang: $clangVer (required: $MIN_CLANG_VERSION+)"
        $needsInstall = $true
    }
    else {
        Write-Success "Clang: $clangVer"
    }
    
    # Check CMake
    $cmakeVer = Get-CMakeVersion
    if ($null -eq $cmakeVer) {
        Write-Warn "CMake: NOT INSTALLED"
        $needsInstall = $true
    }
    elseif ($cmakeVer -lt $MIN_CMAKE_VERSION) {
        Write-Warn "CMake: $cmakeVer (required: $MIN_CMAKE_VERSION+)"
        $needsInstall = $true
    }
    else {
        Write-Success "CMake: $cmakeVer"
    }
    
    # Check Ninja
    $ninjaVer = Get-NinjaVersion
    if ($null -eq $ninjaVer) {
        Write-Warn "Ninja: NOT INSTALLED"
        $needsInstall = $true
    }
    else {
        Write-Success "Ninja: $ninjaVer"
    }
    
    # Check lld
    $hasLLD = Test-LLD
    if (-not $hasLLD) {
        Write-Warn "lld: NOT INSTALLED (included with LLVM)"
        $needsInstall = $true
    }
    else {
        Write-Success "lld: available"
    }
    
    # Check ccache
    $ccacheVer = Get-CcacheVersion
    if ($null -eq $ccacheVer) {
        Write-Warn "ccache: NOT INSTALLED"
        $needsInstall = $true
    }
    elseif ($ccacheVer -lt $MIN_CCACHE_VERSION) {
        Write-Warn "ccache: $ccacheVer (required: $MIN_CCACHE_VERSION+)"
        $needsInstall = $true
    }
    else {
        Write-Success "ccache: $ccacheVer"
    }
    
    # Check LLVM tools (for coverage)
    $hasLLVMTools = Test-LLVMTools
    if (-not $hasLLVMTools) {
        Write-Warn "llvm-profdata/llvm-cov: NOT INSTALLED (needed for coverage)"
        $needsInstall = $true
    }
    else {
        Write-Success "llvm tools: available (for coverage)"
    }
    
    # Check clang-tidy
    $clangTidyVer = Get-ClangTidyVersion
    if ($null -eq $clangTidyVer) {
        Write-Warn "clang-tidy: NOT INSTALLED (needed for static analysis)"
        $needsInstall = $true
    }
    else {
        Write-Success "clang-tidy: $clangTidyVer"
    }
    
    # Check clang-format
    $clangFormatVer = Get-ClangFormatVersion
    if ($null -eq $clangFormatVer) {
        Write-Warn "clang-format: NOT INSTALLED (needed for code formatting)"
        $needsInstall = $true
    }
    else {
        Write-Success "clang-format: $clangFormatVer"
    }
    
    # Install if needed
    if ($needsInstall) {
        Write-Host ""
        Write-Info "Some prerequisites are missing or outdated."
        $response = Read-Host "Would you like to install/update them now? [Y/n]"
        if ($response -notmatch '^[Nn]') {
            if (Install-Prerequisites) {
                Write-Host ""
                Write-Info "Re-checking prerequisites after installation..."
                Write-Host ""
                return Confirm-Prerequisites
            }
            else {
                return $false
            }
        }
        else {
            Write-Err "Cannot continue without prerequisites."
            return $false
        }
    }
    
    return $true
}

function Confirm-Prerequisites {
    $allOk = $true
    
    # Re-check Clang
    $clangVer = Get-ClangVersion
    if ($null -eq $clangVer) {
        Write-Err "Clang: STILL NOT INSTALLED"
        $allOk = $false
    }
    elseif ($clangVer -lt $MIN_CLANG_VERSION) {
        Write-Err "Clang: $clangVer (required: $MIN_CLANG_VERSION+)"
        Show-PathFix "clang" $clangVer $MIN_CLANG_VERSION
        $allOk = $false
    }
    else {
        Write-Success "Clang: $clangVer"
    }
    
    # Re-check CMake
    $cmakeVer = Get-CMakeVersion
    if ($null -eq $cmakeVer) {
        Write-Err "CMake: STILL NOT INSTALLED"
        $allOk = $false
    }
    elseif ($cmakeVer -lt $MIN_CMAKE_VERSION) {
        Write-Err "CMake: $cmakeVer (required: $MIN_CMAKE_VERSION+)"
        Show-PathFix "cmake" $cmakeVer $MIN_CMAKE_VERSION
        $allOk = $false
    }
    else {
        Write-Success "CMake: $cmakeVer"
    }
    
    # Re-check Ninja
    $ninjaVer = Get-NinjaVersion
    if ($null -eq $ninjaVer) {
        Write-Err "Ninja: STILL NOT INSTALLED"
        $allOk = $false
    }
    else {
        Write-Success "Ninja: $ninjaVer"
    }
    
    # Re-check lld
    $hasLLD = Test-LLD
    if (-not $hasLLD) {
        Write-Err "lld: STILL NOT INSTALLED"
        $allOk = $false
    }
    else {
        Write-Success "lld: available"
    }
    
    # Re-check ccache
    $ccacheVer = Get-CcacheVersion
    if ($null -eq $ccacheVer) {
        Write-Err "ccache: STILL NOT INSTALLED"
        $allOk = $false
    }
    elseif ($ccacheVer -lt $MIN_CCACHE_VERSION) {
        Write-Err "ccache: $ccacheVer (required: $MIN_CCACHE_VERSION+)"
        Show-PathFix "ccache" $ccacheVer $MIN_CCACHE_VERSION
        $allOk = $false
    }
    else {
        Write-Success "ccache: $ccacheVer"
    }
    
    # Re-check LLVM tools
    $hasLLVMTools = Test-LLVMTools
    if (-not $hasLLVMTools) {
        Write-Warn "llvm-profdata/llvm-cov: NOT INSTALLED (coverage will not work)"
        # Don't fail for this - it's optional
    }
    else {
        Write-Success "llvm tools: available"
    }
    
    # Re-check clang-tidy
    $clangTidyVer = Get-ClangTidyVersion
    if ($null -eq $clangTidyVer) {
        Write-Err "clang-tidy: STILL NOT INSTALLED"
        $allOk = $false
    }
    else {
        Write-Success "clang-tidy: $clangTidyVer"
    }
    
    # Re-check clang-format
    $clangFormatVer = Get-ClangFormatVersion
    if ($null -eq $clangFormatVer) {
        Write-Err "clang-format: STILL NOT INSTALLED"
        $allOk = $false
    }
    else {
        Write-Success "clang-format: $clangFormatVer"
    }
    
    if (-not $allOk) {
        Write-Host ""
        Write-Err "Some prerequisites still don't meet requirements."
        Write-Err "Please fix the issues above and re-run this script."
        return $false
    }
    
    Write-Host ""
    Write-Success "All prerequisites verified!"
    return $true
}

# Main script starts here
Write-Host ""
Write-Host "========================================"
Write-Host "  MyProject Setup Script (Windows)"
Write-Host "========================================"

# Check prerequisites first (unless skipped)
if (-not $SkipPrereqs) {
    if (-not (Test-Prerequisites)) {
        exit 1
    }
}

# Derive variants
$ProjectUpper = $Name.ToUpper()
$ProjectLower = $Name.ToLower()

Write-Host ""
Write-Info "========================================"
Write-Info "  Configuring Project"
Write-Info "========================================"
Write-Host ""
Write-Info "Setting up project: $Name"
Write-Info "  Uppercase: $ProjectUpper"
Write-Info "  Lowercase: $ProjectLower"
if ($Author) { Write-Info "  Author: $Author" }
Write-Host ""

# Get all text files, excluding .git, build, dist, coverage, and setup scripts
$files = Get-ChildItem -Path $ScriptDir -Recurse -File |
    Where-Object {
        $_.FullName -notmatch "[\\/]\.git[\\/]" -and
        $_.FullName -notmatch "[\\/]build[\\/]" -and
        $_.FullName -notmatch "[\\/]dist[\\/]" -and
        $_.FullName -notmatch "[\\/]coverage[\\/]" -and
        $_.Name -ne "setup.sh" -and
        $_.Name -ne "setup.ps1"
    }

$count = 0

foreach ($file in $files) {
    try {
        $content = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
        if (-not $content) { continue }
        
        $modified = $false
        $newContent = $content
        
        # Replace MyProject -> ProjectName
        if ($newContent -match "MyProject") {
            $newContent = $newContent -replace "MyProject", $Name
            $modified = $true
        }
        
        # Replace MYPROJECT -> PROJECTNAME
        if ($newContent -match "MYPROJECT") {
            $newContent = $newContent -replace "MYPROJECT", $ProjectUpper
            $modified = $true
        }
        
        # Replace myproject -> projectname
        if ($newContent -match "myproject") {
            $newContent = $newContent -replace "myproject", $ProjectLower
            $modified = $true
        }
        
        # Update author if provided
        if ($Author -and ($newContent -match "Your Name")) {
            $newContent = $newContent -replace "Your Name", $Author
            $modified = $true
        }
        
        if ($modified) {
            Set-Content -Path $file.FullName -Value $newContent -NoNewline
            $count++
            if ($ShowDetails) {
                $relativePath = $file.FullName.Substring($ScriptDir.Length + 1)
                Write-Host "  Updated: $relativePath"
            }
        }
    }
    catch {
        # Skip files that can't be read as text
    }
}

Write-Host ""
Write-Success "Updated $count files."

# Clean up setup scripts
Write-Info "Removing setup scripts..."
$setupSh = Join-Path $ScriptDir "setup.sh"
$setupPs1 = Join-Path $ScriptDir "setup.ps1"
if (Test-Path $setupSh) { Remove-Item $setupSh -Force }
if (Test-Path $setupPs1) { Remove-Item $setupPs1 -Force }

Write-Host ""
Write-Success "Setup complete! Your project '$Name' is ready."
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Review the changes: git diff"
Write-Host "  2. Build the project: cmake --preset win-debug; cmake --build --preset win-debug"
Write-Host "  3. Run tests: ctest --preset win-debug"
Write-Host "  4. Commit: git add -A; git commit -m 'Initial project setup'"
