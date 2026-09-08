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
    [string]$LlvmVersion = "22.1.7",
    # CMake/Ninja pinned to match what the windows-2025 GitHub Actions runner image ships
    # (confirmed against actions/runner-images' Windows2025-Readme.md), so a fresh dev-box
    # setup and CI land on the same versions. ccache has no CI-side winget equivalent to
    # mirror (CI installs it via Chocolatey instead, pinned separately in
    # .github/actions/setup-windows-llvm/action.yml), so this pins the latest version winget
    # actually has available.
    [string]$CMakeVersion = "3.31.6",
    [string]$NinjaVersion = "1.13.2",
    [string]$CcacheVersion = "4.14",
    # Renovate (.github/renovate.json5) bumps this literal directly; every consumer below
    # (the winget --id, Resolve-Python's install-path probing, and Invoke-Python's log text)
    # derives from it instead of hardcoding "3.14"/"314" a second time, so a version bump can't
    # desync the winget package ID from the paths this script searches for afterward.
    [string]$PythonVersion = "3.14"
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Invoke-WinGet {
    param(
        [Parameter(Position = 0, ValueFromRemainingArguments)]
        [string[]]$Arguments
    )
    if ($DryRun) {
        Write-Host "[dry-run] winget $($Arguments -join ' ')"
    } else {
        winget @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "winget failed with exit code ${LASTEXITCODE}: winget $($Arguments -join ' ')"
        }
    }
}

function Resolve-Python {
    $versionCompact = $PythonVersion -replace '\.', ''
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python$versionCompact\python.exe"),
        (Join-Path $env:ProgramFiles "Python$versionCompact\python.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $command = Get-Command "python$PythonVersion" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "Python $PythonVersion was installed but its executable could not be located. Restart the terminal and rerun this script."
}

function Invoke-Python {
    param(
        [Parameter(Position = 0, ValueFromRemainingArguments)]
        [string[]]$Arguments
    )
    if ($DryRun) {
        Write-Host "[dry-run] python$PythonVersion $($Arguments -join ' ')"
        return
    }

    & $script:PythonExecutable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python failed with exit code ${LASTEXITCODE}: python$PythonVersion $($Arguments -join ' ')"
    }
}

Write-Host "=== TaskSmack Dev Setup (Windows) ==="
Write-Host "LLVM version: $LlvmVersion"
Write-Host "CMake version: $CMakeVersion"
Write-Host "Ninja version: $NinjaVersion"
Write-Host "ccache version: $CcacheVersion"
Write-Host "Python version: $PythonVersion"
if ($DryRun) { Write-Host "(dry-run mode — nothing will be installed)" }
Write-Host ""

# ── Verify winget is available ────────────────────────────────────────────────
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install it from the Microsoft Store or update Windows."
}

# ── Step 1: Core build tools ─────────────────────────────────────────────────
# Microsoft.VisualStudio.Workload.VCTools pulls in a Windows SDK (including rc.exe) as
# one of its default components -- no separate SDK package is needed. CMakeLists.txt
# fails configuration with a clear error if rc.exe can't be found, since TaskSmack's
# icon and version info depend on it (the DPI-awareness manifest is embedded
# independently at link time and doesn't need rc.exe).
Write-Host "==> Installing Visual Studio C++ Build Tools and Windows SDK..."
Invoke-WinGet install --id Microsoft.VisualStudio.2022.BuildTools --source winget --silent --accept-package-agreements --accept-source-agreements --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

Write-Host "==> Installing CMake $CMakeVersion..."
Invoke-WinGet install --id Kitware.CMake --version $CMakeVersion --source winget --silent --accept-package-agreements --accept-source-agreements

Write-Host "==> Installing Ninja $NinjaVersion..."
Invoke-WinGet install --id Ninja-build.Ninja --version $NinjaVersion --source winget --silent --accept-package-agreements --accept-source-agreements

Write-Host "==> Installing Python $PythonVersion..."
Invoke-WinGet install --id "Python.Python.$PythonVersion" --source winget --silent --accept-package-agreements --accept-source-agreements
if (-not $DryRun) {
    $script:PythonExecutable = Resolve-Python
}

# ── Step 2: LLVM / Clang ─────────────────────────────────────────────────────
Write-Host ""
Write-Host "==> Installing LLVM $LlvmVersion..."
Invoke-WinGet install --id LLVM.LLVM --version $LlvmVersion --source winget --silent --accept-package-agreements --accept-source-agreements

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
Write-Host "==> Installing ccache $CcacheVersion..."
Invoke-WinGet install --id ccache.ccache --version $CcacheVersion --source winget --silent --accept-package-agreements --accept-source-agreements

# ── Step 4: jinja2 for GLAD ─────────────────────────────────────────────────
Write-Host ""
Write-Host "==> Installing Python packages (jinja2 for GLAD generation)..."
Invoke-Python @("-m", "pip", "install", "--require-hashes", "-r", (Join-Path $RepoRoot "requirements-glad.lock"))

if (-not $Minimal) {
    # ── Step 5: Development Python dependencies ─────────────────────────────
    Write-Host ""
    Write-Host "==> Installing development Python dependencies..."
    Invoke-Python @("-m", "pip", "install", "-r", (Join-Path $RepoRoot "requirements.txt"))
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
