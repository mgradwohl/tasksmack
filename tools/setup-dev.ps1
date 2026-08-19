# tools/setup-dev.ps1 — Install TaskSmack development prerequisites on Windows.
#
# Usage:
#   pwsh tools/setup-dev.ps1              # Install all prerequisites (requires winget)
#   pwsh tools/setup-dev.ps1 -DryRun      # Print what would be installed without running winget
#   pwsh tools/setup-dev.ps1 -Minimal     # Install build prerequisites only
#
# After running this script, verify your environment with: pwsh tools/check-prereqs.ps1
#
# See CONTRIBUTING.md for full documentation of prerequisites.
#
# Note: LLVM_ROOT must be set to C:\Program Files\LLVM after installation.
# Run this script from an elevated (Administrator) PowerShell session.

[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Minimal,
    [string]$LlvmVersion = "22.1.0"
)

$ErrorActionPreference = 'Stop'

function Invoke-WinGet {
    param([string[]]$Args)
    if ($DryRun) {
        Write-Host "[dry-run] winget $($Args -join ' ')"
    } else {
        winget @Args
    }
}

Write-Host "=== TaskSmack Dev Setup (Windows) ==="
Write-Host "LLVM version: $LlvmVersion"
if ($DryRun) { Write-Host "(dry-run mode — nothing will be installed)" }
Write-Host ""

# ── Verify winget is available ────────────────────────────────────────────────
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Error "winget not found. Install it from the Microsoft Store or update Windows."
}

# ── Step 1: Core build tools ─────────────────────────────────────────────────
Write-Host "==> Installing CMake..."
Invoke-WinGet install --id Kitware.CMake --source winget --silent --accept-package-agreements --accept-source-agreements

Write-Host "==> Installing Ninja..."
Invoke-WinGet install --id Ninja-build.Ninja --source winget --silent --accept-package-agreements --accept-source-agreements

Write-Host "==> Installing Python 3..."
Invoke-WinGet install --id Python.Python.3.13 --source winget --silent --accept-package-agreements --accept-source-agreements

# ── Step 2: LLVM / Clang ─────────────────────────────────────────────────────
Write-Host ""
Write-Host "==> Installing LLVM $LlvmVersion..."
$llvmId = "LLVM.LLVM.$LlvmVersion"
Invoke-WinGet install --id $llvmId --source winget --silent --accept-package-agreements --accept-source-agreements

Write-Host ""
Write-Host "==> Setting LLVM_ROOT environment variable..."
$llvmPath = "C:\Program Files\LLVM"
if ($DryRun) {
    Write-Host "[dry-run] [System.Environment]::SetEnvironmentVariable('LLVM_ROOT', '$llvmPath', 'Machine')"
} else {
    [System.Environment]::SetEnvironmentVariable('LLVM_ROOT', $llvmPath, 'Machine')
    $env:LLVM_ROOT = $llvmPath
    Write-Host "  LLVM_ROOT set to: $llvmPath"
}

# ── Step 3: ccache ───────────────────────────────────────────────────────────
Write-Host ""
Write-Host "==> Installing ccache..."
Invoke-WinGet install --id ccache.ccache --source winget --silent --accept-package-agreements --accept-source-agreements

if (-not $Minimal) {
    # ── Step 4: jinja2 for GLAD ─────────────────────────────────────────────
    Write-Host ""
    Write-Host "==> Installing Python packages (jinja2 for GLAD generation)..."
    if ($DryRun) {
        Write-Host "[dry-run] pip install jinja2"
    } else {
        pip install jinja2
    }

    # ── Step 5: pre-commit ──────────────────────────────────────────────────
    Write-Host ""
    Write-Host "==> Installing pre-commit (optional but recommended)..."
    if ($DryRun) {
        Write-Host "[dry-run] pip install pre-commit"
    } else {
        pip install pre-commit
    }
}

Write-Host ""
Write-Host "=== Setup complete ==="
Write-Host ""
Write-Host "IMPORTANT: Restart your terminal (or log out/in) for PATH changes to take effect."
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Verify environment:  pwsh tools/check-prereqs.ps1"
Write-Host "  2. Configure project:   cmake --preset win-debug"
Write-Host "  3. Build:               cmake --build --preset win-debug"
Write-Host "  4. Run tests:           ctest --preset win-debug"
Write-Host ""
Write-Host "For full documentation see CONTRIBUTING.md"
