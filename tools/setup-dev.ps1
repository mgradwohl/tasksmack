# tools/setup-dev.ps1 — Install TaskSmack development prerequisites on Windows.
#
# Usage:
#   pwsh tools/setup-dev.ps1              # Install what building and testing need (requires winget)
#   pwsh tools/setup-dev.ps1 -DevTools    # Also install the optional developer tools (ccache, pre-commit, ...)
#   pwsh tools/setup-dev.ps1 -DryRun      # Print what would be installed without running winget
#
# After running this script, verify your environment with: pwsh tools/check-prereqs.ps1
#
# See CONTRIBUTING.md for full documentation of prerequisites.
#
# Needs winget and PowerShell 7; a clean Windows gets PowerShell 7 with
# `winget install --id Microsoft.PowerShell --exact --source winget` (CONTRIBUTING.md, Automated Setup (Windows)).
# Run it from a normal PowerShell 7 terminal: the installers ask for administrator approval (UAC)
# themselves. It sets LLVM_ROOT to C:\Program Files\LLVM, machine-wide from an elevated terminal and
# for the current user otherwise.

[CmdletBinding()]
param(
    [switch]$DryRun,
    # Optional developer tools, not needed to build, test or run: ccache (faster rebuilds) and
    # requirements.txt (pre-commit and the other Python dev tools).
    [switch]$DevTools,
    # Before #2618 everything was installed by default and -Minimal skipped the dev tools; the
    # default is now what -Minimal was, so it is accepted and does nothing.
    [switch]$Minimal,
    # The standalone Windows SDK (winget Microsoft.WindowsSDK.10.0.26100). 26100 is the SDK the
    # windows-2025 CI image builds with. 10.0.26100.7463 is the newest build whose winget manifest
    # installs on Windows 10 and Windows 11 before 24H2; 10.0.26100.7705's requires 24H2. The
    # headers and libraries install to Windows Kits\10\{Include,Lib}\10.0.26100.0 either way.
    [string]$WindowsSdkVersion = "10.0.26100.7463",
    [string]$LlvmVersion = "23.1.3",
    # Ninja pinned to match what the windows-2025 GitHub Actions runner image ships (confirmed
    # against actions/runner-images' Windows2025-Readme.md), so a fresh dev-box setup and CI land
    # on the same version (CI verifies the image's ninja version in
    # .github/actions/setup-windows-llvm/action.yml). CMake was pinned the same way until winget's
    # catalog dropped every 3.x, the image's 3.31.x included (#2844); it now pins the newest
    # version winget carries (the project needs 3.29+, CMakeLists.txt). ccache has no CI-side
    # winget equivalent to mirror (CI gets it from hendrikmuhs/ccache-action's own pinned,
    # checksum-verified release binary), so this pins the latest version winget actually has
    # available. winget keeps only recent versions of a package, so a pin it no longer carries
    # installs its newest version instead, with a warning (Install-WinGetPackage below).
    [string]$CMakeVersion = "4.4.4",
    [string]$NinjaVersion = "1.13.2",
    [string]$CcacheVersion = "4.14.2",
    # Renovate (.github/renovate.json5) bumps this literal directly; every consumer below
    # (the winget --id, Resolve-Python's install-path probing, and Initialize-Venv's log text)
    # derives from it instead of hardcoding "3.14"/"314" a second time, so a version bump can't
    # desync the winget package ID from the paths this script searches for afterward.
    [string]$PythonVersion = "3.14"
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common.ps1")

# The project-local Python environment, as tools/setup-dev.sh creates on Linux. Windows venvs keep
# the interpreter in Scripts\ (Linux: bin/); cmake/Dependencies.cmake picks it up from there.
$VenvDir = Join-Path $RepoRoot ".venv"
$VenvPython = Join-Path $VenvDir "Scripts\python.exe"

# The presets and CI build with one LLVM major (the clang-format rev pinned in .pre-commit-config.yaml);
# reject any other, as setup-dev.sh's --llvm guard does.
$SupportedLlvmMajor = Get-PinnedLlvmMajor
$requestedLlvmMajor = if ($LlvmVersion -match '^(\d+)') { [int]$Matches[1] } else { $null }
if ($SupportedLlvmMajor -and $requestedLlvmMajor -ne $SupportedLlvmMajor) {
    throw "TaskSmack presets currently require LLVM $SupportedLlvmMajor; received -LlvmVersion $LlvmVersion."
}

# winget exit codes (https://github.com/microsoft/winget-cli/blob/master/doc/windows/package-manager/winget/returnCodes.md)
# that mean the package is already there, so a re-run of this script doesn't throw on them:
#   0x8A15002B APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE      -- installed, no applicable upgrade
#   0x8A150061 APPINSTALLER_CLI_ERROR_PACKAGE_ALREADY_INSTALLED  -- package is already installed
# PowerShell reads these hex literals as the same negative Int32 values $LASTEXITCODE carries.
$WinGetAlreadyInstalledCodes = @(0x8A15002B, 0x8A150061)
# 0x8A15010D APPINSTALLER_CLI_ERROR_INSTALL_ALREADY_INSTALLED -- another version is already installed.
# Not fatal, but the installed version is not the pinned one; check-prereqs.ps1 reports which it is.
$WinGetOtherVersionInstalledCode = 0x8A15010D
# 0x8A150017 APPINSTALLER_CLI_ERROR_NO_MANIFEST_FOUND ("No version found matching: <pin>") -- the
# package exists but the catalog no longer carries the pinned version; Install-WinGetPackage falls
# back to the newest it has (#2844).
$WinGetNoManifestFoundCode = 0x8A150017
# The switches every install here uses: the winget source only (not msstore), silent, and both
# agreements accepted up front so nothing prompts.
$WinGetInstallSwitches = @('--source', 'winget', '--silent', '--accept-package-agreements', '--accept-source-agreements')

# Run winget, as the user sees it. A nonzero exit code throws, except the already-installed codes
# above (not an error on a re-run) and any in -ReturnCodes, on which the function returns with
# the code left in $LASTEXITCODE for the caller to look at.
function Invoke-WinGet {
    param(
        [int[]]$ReturnCodes = @(),
        [Parameter(Position = 0, ValueFromRemainingArguments)]
        [string[]]$Arguments
    )
    if ($DryRun) {
        Write-Host "[dry-run] winget $($Arguments -join ' ')"
        return
    }

    winget @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0 -or $ReturnCodes -contains $exitCode) {
        return
    }
    $hexCode = '0x{0:X8}' -f $exitCode
    if ($WinGetAlreadyInstalledCodes -contains $exitCode) {
        Write-Host "  Already installed (winget $hexCode); continuing."
        return
    }
    if ($exitCode -eq $WinGetOtherVersionInstalledCode) {
        Write-Warning "winget $hexCode`: another version is already installed (winget $($Arguments -join ' ')). Continuing; run pwsh tools/check-prereqs.ps1 to see which version is in use."
        return
    }
    throw "winget failed with exit code $hexCode ($exitCode): winget $($Arguments -join ' ')"
}

# The installed version of a winget package as `winget list` reports it, or $null when that can't
# be read. The package's row holds its Id and then its version; only the headers are localized.
# No --exact: it makes the Id case-sensitive, and the ids here follow the catalog's spelling loosely.
function Get-WinGetInstalledVersion {
    param([Parameter(Mandatory)][string]$Id)
    $lines = @(winget list --id $Id --source winget --accept-source-agreements --disable-interactivity 2>$null)
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    foreach ($line in $lines) {
        $tokens = @(($line -split '\s+') | Where-Object { $_ })
        for ($i = 0; $i -lt $tokens.Count - 1; $i++) {
            if ($tokens[$i] -ieq $Id) {
                return $tokens[$i + 1]
            }
        }
    }
    return $null
}

# Install a winget package, at its pinned version when -Version is given. winget's catalog keeps
# only recent versions of a package, so a pin that was right when written stops installing after a
# while (CMake 3.31.12 was gone by 2026-10, with every other 3.x), and the script used to stop on the
# first such tool (#2844). The pin stays the preferred version, for a setup that matches the
# documented one; when the catalog no longer has it (0x8A150017), this warns, installs the newest
# version winget has instead, and prints the version that got installed. -Arguments are further
# winget install switches (--override), after the common ones.
function Install-WinGetPackage {
    param(
        [Parameter(Mandatory)][string]$Id,
        [string]$Version,
        [string[]]$Arguments = @()
    )
    if (-not $Version) {
        Invoke-WinGet install --id $Id @WinGetInstallSwitches @Arguments
        return
    }
    Invoke-WinGet -ReturnCodes $WinGetNoManifestFoundCode install --id $Id --version $Version @WinGetInstallSwitches @Arguments
    if ($DryRun) {
        Write-Host "[dry-run]   (if $Version is no longer in the winget catalog: a warning, then the same install without --version)"
        return
    }
    if ($LASTEXITCODE -ne $WinGetNoManifestFoundCode) {
        return
    }
    Write-Warning "winget 0x8A150017: $Id $Version is no longer in the winget catalog; installing the newest version winget has instead (#2844). Move the pin in tools/setup-dev.ps1 to a version the catalog carries: winget show --id $Id --source winget --versions"
    Invoke-WinGet install --id $Id @WinGetInstallSwitches @Arguments
    $installed = Get-WinGetInstalledVersion -Id $Id
    if ($installed) {
        Write-Host "  $Id $installed is installed (the pin was $Version)."
    }
    else {
        Write-Host "  $Id installed (the pin was $Version); run pwsh tools/check-prereqs.ps1 to see which version is in use."
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

# Run the project venv's Python.
function Invoke-VenvPython {
    param(
        [Parameter(Position = 0, ValueFromRemainingArguments)]
        [string[]]$Arguments
    )
    if ($DryRun) {
        Write-Host "[dry-run] $VenvPython $($Arguments -join ' ')"
        return
    }

    & $VenvPython @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python failed with exit code ${LASTEXITCODE}: $VenvPython $($Arguments -join ' ')"
    }
}

# Create .venv with the installed Python $PythonVersion, unless a Windows venv is already there.
function Initialize-Venv {
    if (Test-Path -LiteralPath $VenvPython) {
        Write-Host "  Using existing virtual environment: $VenvDir"
        return
    }
    if (Test-Path -LiteralPath $VenvDir) {
        # Most likely a Linux venv (.venv/bin/python) from WSL in the same checkout. Recreating it
        # in place would break it for Linux, so leave the choice to the developer.
        if ($DryRun) {
            Write-Warning "$VenvDir exists but has no Scripts\python.exe (a Linux venv from WSL?); a real run would stop here."
            return
        }
        throw "$VenvDir exists but has no Scripts\python.exe (a Linux venv from WSL?). Remove or rename it, then rerun this script."
    }
    if ($DryRun) {
        Write-Host "[dry-run] python$PythonVersion -m venv $VenvDir"
        return
    }

    & $script:PythonExecutable -m venv $VenvDir
    if ($LASTEXITCODE -ne 0) {
        throw "Creating the virtual environment failed with exit code ${LASTEXITCODE}: $script:PythonExecutable -m venv $VenvDir"
    }
    Write-Host "  Created virtual environment: $VenvDir"
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
# clang (LLVM_ROOT) targets the MSVC ABI, so it compiles against the MSVC C++ standard library
# and CRT and the Windows SDK, and links their import and static libraries. It finds the MSVC
# libraries through the Visual Studio installation, and the SDK through the Windows Kits
# "Installed Roots" registry key, which the standalone SDK installer writes as Visual Studio's
# does. The MSVC libraries are the only thing the build takes from Visual Studio, so install
# just that component rather than the VCTools workload with its recommended components (#2618,
# #2646): CMake and Ninja are the standalone ones below, the resource compiler is LLVM's llvm-rc
# (cmake/RCCompiler.cmake), and MSBuild, the ATL, the test adapters and the bundled CMake are
# never used. The workload with --includeRecommended was 346 packages (131 MSIs).
$isArm64 = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'
$vsComponents = @('Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
if ($isArm64) {
    # Windows on Arm builds natively (#2214), against the ARM64 libraries.
    $vsComponents += 'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
}
$vsAdd = ($vsComponents | ForEach-Object { "--add $_" }) -join ' '
Write-Host "==> Installing Visual Studio C++ Build Tools (MSVC libraries only)..."
Install-WinGetPackage -Id Microsoft.VisualStudio.2022.BuildTools -Arguments @('--override', "--wait --passive --norestart $vsAdd")

# The Windows SDK, standalone and with only the desktop C++ headers and libraries for this
# machine's architecture (#2661). Visual Studio's Windows11SDK component can't pick SDK features:
# it installs every feature group (App Verifier, UWP, the Logo Toolkit, signing and MSI tools,
# and the x86, x64 and ARM64 libraries), 116 MSIs that took about 2 h 15 min in Windows Sandbox.
# The build uses Include\<sdk>\{ucrt,um,shared} and Lib\<sdk>\{ucrt,um}\<arch>, and no SDK tool.
# Skipped when those are already there, as they are with Visual Studio's SDK component.
$sdkBuild = ($WindowsSdkVersion -split '\.')[0..2] -join '.'
$sdkDir = "$sdkBuild.0"
$sdkArch = if ($isArm64) { 'arm64' } else { 'x64' }
$sdkFeature = if ($isArm64) { 'OptionId.DesktopCPParm64' } else { 'OptionId.DesktopCPPx64' }

# The Windows 10 SDK root (Windows Kits\10), from the registry key clang reads, else the default.
function Get-WindowsKitsRoot {
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots') {
        $root = (Get-ItemProperty -LiteralPath $key -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
        if ($root) { return $root }
    }
    return Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
}

# The SDK files the build needs that are missing, as paths; none means the SDK is ready.
function Get-MissingWindowsSdkFile {
    $kits = Get-WindowsKitsRoot
    @(
        "Include\$sdkDir\um\Windows.h",
        "Include\$sdkDir\shared\sdkddkver.h",
        "Include\$sdkDir\ucrt\stdio.h",
        "Lib\$sdkDir\um\$sdkArch\kernel32.lib",
        "Lib\$sdkDir\ucrt\$sdkArch\ucrt.lib"
    ) | ForEach-Object { Join-Path $kits $_ } | Where-Object { -not (Test-Path -LiteralPath $_) }
}

$missingSdkFiles = @(Get-MissingWindowsSdkFile)
if ($missingSdkFiles.Count -eq 0) {
    Write-Host "==> Windows SDK $sdkDir desktop C++ headers and $sdkArch libraries already in $(Get-WindowsKitsRoot); skipping."
}
else {
    Write-Host "==> Installing Windows SDK $WindowsSdkVersion (desktop C++ $sdkArch headers and libraries only)..."
    # --override replaces the manifest's /q, so it carries the quiet switches itself. The log is
    # what tools/newcomer-check.ps1's evidence step counts the SDK's packages from.
    $sdkLog = Join-Path ([IO.Path]::GetTempPath()) 'tasksmack-winsdksetup.log'
    $sdkStart = Get-Date
    Install-WinGetPackage -Id "Microsoft.WindowsSDK.$sdkBuild" -Version $WindowsSdkVersion -Arguments @('--override', "/features $sdkFeature /quiet /norestart /ceip off /log `"$sdkLog`"")
    if (-not $DryRun) {
        $missingSdkFiles = @(Get-MissingWindowsSdkFile)
        if ($missingSdkFiles.Count -gt 0) {
            throw "The Windows SDK install finished, but these files are missing: $($missingSdkFiles -join ', '). See $sdkLog."
        }
        Write-Host ("  Windows SDK installed in {0:N0} min." -f ((Get-Date) - $sdkStart).TotalMinutes)
    }
}

Write-Host "==> Installing CMake $CMakeVersion..."
Install-WinGetPackage -Id Kitware.CMake -Version $CMakeVersion

Write-Host "==> Installing Ninja $NinjaVersion..."
Install-WinGetPackage -Id Ninja-build.Ninja -Version $NinjaVersion

Write-Host "==> Installing Python $PythonVersion..."
Install-WinGetPackage -Id "Python.Python.$PythonVersion"
if (-not $DryRun) {
    $script:PythonExecutable = Resolve-Python
}

# ── Step 2: LLVM / Clang ─────────────────────────────────────────────────────
Write-Host ""
Write-Host "==> Installing LLVM $LlvmVersion..."
Install-WinGetPackage -Id LLVM.LLVM -Version $LlvmVersion

Write-Host ""
Write-Host "==> Setting LLVM_ROOT environment variable..."
$llvmPath = "C:\Program Files\LLVM"
# A machine-wide variable needs an elevated shell; without one, set it for the current user instead
# of failing here, after LLVM is installed but before the Python packages.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$envScope = if ($isAdmin) { 'Machine' } else { 'User' }
if ($DryRun) {
    Write-Host "[dry-run] [System.Environment]::SetEnvironmentVariable('LLVM_ROOT', '$llvmPath', '$envScope')"
} else {
    [System.Environment]::SetEnvironmentVariable('LLVM_ROOT', $llvmPath, $envScope)
    $env:LLVM_ROOT = $llvmPath
    Write-Host "  LLVM_ROOT set to: $llvmPath ($envScope scope)"
}

# ── Step 3: Project venv + jinja2 for GLAD ──────────────────────────────────
# Into .venv, not the global Python, as setup-dev.sh does on Linux.
Write-Host ""
Write-Host "==> Creating the project Python environment (.venv)..."
Initialize-Venv
Invoke-VenvPython @("-m", "pip", "install", "--upgrade", "pip")

Write-Host ""
Write-Host "==> Installing Python packages (jinja2 for GLAD generation)..."
Invoke-VenvPython @("-m", "pip", "install", "--require-hashes", "-r", (Join-Path $RepoRoot "requirements-glad.txt"))

if ($DevTools) {
    # ── Step 4 (-DevTools): optional developer tools ───────────────────────
    Write-Host ""
    Write-Host "==> Installing ccache $CcacheVersion (optional: faster rebuilds)..."
    Install-WinGetPackage -Id ccache.ccache -Version $CcacheVersion

    Write-Host ""
    Write-Host "==> Installing development Python dependencies (pre-commit and others)..."
    Invoke-VenvPython @("-m", "pip", "install", "-r", (Join-Path $RepoRoot "requirements.txt"))
}

Write-Host ""
Write-Host "=== Setup complete ==="
Write-Host ""
Write-Host "IMPORTANT: Open a new PowerShell 7 (pwsh) terminal for the PATH and LLVM_ROOT changes to take effect."
Write-Host ""
Write-Host "Next steps, in the new terminal, from the repository root:"
Write-Host "  1. Activate Python:        .venv\Scripts\Activate.ps1"
Write-Host "  2. Verify environment:     pwsh tools/check-prereqs.ps1"
Write-Host "  3. Configure, build, test: cmake --workflow --preset win-dev"
if ($DevTools) {
    Write-Host "  4. Install git hooks:      pre-commit install"
}
else {
    Write-Host "  Before your first pull request: pwsh tools/setup-dev.ps1 -DevTools, then pre-commit install"
}
Write-Host ""
Write-Host "For full documentation see CONTRIBUTING.md"
