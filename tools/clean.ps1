# tools/clean.ps1 — Remove TaskSmack build artifacts and caches (Windows).
#
# Usage:
#   pwsh tools/clean.ps1              # Remove build/ and coverage/ only
#   pwsh tools/clean.ps1 -All        # Also remove .cache/ (FetchContent downloads)
#   pwsh tools/clean.ps1 -DryRun     # Print what would be removed without deleting
#   pwsh tools/clean.ps1 -Yes        # Skip confirmation prompt

[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$All,
    [switch]$DryRun,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$targets = @(
    (Join-Path $RepoRoot "build"),
    (Join-Path $RepoRoot "coverage"),
    (Join-Path $RepoRoot "dist"),
    (Join-Path $RepoRoot "compile_commands.json"),
    (Join-Path $RepoRoot "compile_commands_notidy.json")
)
if ($All) {
    $targets += (Join-Path $RepoRoot ".cache")
    $targets += (Join-Path $RepoRoot "profiles")
}

Write-Host "The following paths will be removed:"
foreach ($t in $targets) {
    if (Test-Path $t) {
        Write-Host "  $t"
    }
}
Write-Host ""

if (-not $Yes -and -not $DryRun) {
    $answer = Read-Host "Proceed? [y/N]"
    if ($answer -notmatch '^[yY]') {
        Write-Host "Aborted."
        exit 0
    }
}

foreach ($t in $targets) {
    if (Test-Path $t) {
        if ($DryRun) {
            Write-Host "[dry-run] Remove-Item -Recurse -Force $t"
        } else {
            Write-Host "Removing: $t"
            Remove-Item -Recurse -Force $t
        }
    }
}

if ($DryRun) {
    Write-Host "Dry run complete. Nothing was removed."
} else {
    Write-Host "Clean complete."
}
